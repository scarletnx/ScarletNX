/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* An AOT module's descriptor without LLVM, for a module whose functions the x86-64 back end made (module_llvm.c):
   the same symbols and data module_llvm.c's emit_module, emit_routes, emit_nros and emit_imports give, written by
   x64_elf.h's builder, their few functions as bytes. The data's layout is module_abi.h's and nro_slots.h's C types,
   the host's own (this is the host's module) */
#include "nx/loader/module_x64.h"
#include "nx/loader/module.h"
#include "nx/loader/module_abi.h"
#include "nx/loader/hook_routes.h"
#include "nx/loader/loader.h"
#include "common/cpu/recompiler/x64/x64_elf.h"
#include "common/runtime/interface.h"
#include "common/utilities/bits.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__x86_64__) && !defined(_WIN32)
#include <elf.h>

/* ---- code bytes into .text ---- */
static void b8(SnxXElf* o, unsigned v) {
    unsigned char c = (unsigned char)v;
    snx_xelf_put(o, SNX_ELF_TEXT, &c, 1, 1);
}
static void b32(SnxXElf* o, SnxU32 v) { snx_xelf_put(o, SNX_ELF_TEXT, &v, 4, 1); }
/* LEA reg, [RIP + sym] (reg 0-15) */
static void lea(SnxXElf* o, int reg, const char* sym) {
    size_t at;
    b8(o, 0x48 | (reg >> 3) << 2);
    b8(o, 0x8D);
    b8(o, 0x05 | (reg & 7) << 3);
    at = snx_xelf_put(o, SNX_ELF_TEXT, NULL, 4, 1);
    snx_xelf_reloc(o, SNX_ELF_TEXT, at, sym, R_X86_64_PC32, -4);
}
/* a function of flags at the code to come */
static size_t fn_begin(SnxXElf* o, const char* name, int flags) {
    size_t at = snx_xelf_put(o, SNX_ELF_TEXT, NULL, 0, 16);
    snx_xelf_symbol(o, name, SNX_ELF_TEXT, at, 0, flags | SNX_ELF_FUNC);
    return at;
}
/* const T* f(void) { return &sym; } */
static void returns(SnxXElf* o, const char* name, const char* sym) {
    fn_begin(o, name, SNX_ELF_GLOBAL);
    lea(o, 0, sym);
    b8(o, 0xC3);
}
/* a 64-bit pointer at offset of section, to sym */
static void ptr(SnxXElf* o, int section, size_t at, const char* sym) {
    snx_xelf_reloc(o, section, at, sym, R_X86_64_64, 0);
}
/* a string in .rodata, its local symbol name */
static void string(SnxXElf* o, const char* text, const char* name) {
    size_t at = snx_xelf_put(o, SNX_ELF_RODATA, text, strlen(text) + 1, 1);
    snx_xelf_symbol(o, name, SNX_ELF_RODATA, at, strlen(text) + 1, SNX_ELF_LOCAL);
}
static void fn_name(char* out, size_t size, SnxU64 address) {
    snprintf(out, size, "F_%llX", (unsigned long long)address);
}

/* snx_module(): ENT_ADDR[i] = BASE + ENT_OFF[i], ENT_PTR[i] = FNS[ENT_FN[i]] for each entry (as emit_module's loop:
   no relocation for each), then &MOD */
static void module_fn(SnxXElf* o, size_t count) {
    size_t loop, jae, jmp, done;
    fn_begin(o, "snx_module", SNX_ELF_GLOBAL);
    lea(o, 6, "ENT_OFF");  /* rsi */
    lea(o, 7, "ENT_FN");   /* rdi */
    lea(o, 8, "ENT_ADDR"); /* r8 */
    lea(o, 9, "ENT_PTR");  /* r9 */
    lea(o, 10, "FNS");     /* r10 */
    b8(o, 0x49), b8(o, 0xBB);
    {
        SnxU64 base = SNX_CODE_BASE;
        snx_xelf_put(o, SNX_ELF_TEXT, &base, 8, 1); /* mov r11, BASE */
    }
    b8(o, 0x31), b8(o, 0xC9); /* xor ecx, ecx */
    loop = snx_xelf_put(o, SNX_ELF_TEXT, NULL, 0, 1);
    b8(o, 0x81), b8(o, 0xF9), b32(o, (SnxU32)count); /* cmp ecx, count */
    b8(o, 0x0F), b8(o, 0x83);                        /* jae done */
    jae = snx_xelf_put(o, SNX_ELF_TEXT, NULL, 4, 1);
    b8(o, 0x8B), b8(o, 0x14), b8(o, 0x8E);            /* mov edx, [rsi + rcx*4] */
    b8(o, 0x4C), b8(o, 0x01), b8(o, 0xDA);            /* add rdx, r11 */
    b8(o, 0x49), b8(o, 0x89), b8(o, 0x14), b8(o, 0xC8); /* mov [r8 + rcx*8], rdx */
    b8(o, 0x8B), b8(o, 0x14), b8(o, 0x8F);            /* mov edx, [rdi + rcx*4] */
    b8(o, 0x49), b8(o, 0x8B), b8(o, 0x14), b8(o, 0xD2); /* mov rdx, [r10 + rdx*8] */
    b8(o, 0x49), b8(o, 0x89), b8(o, 0x14), b8(o, 0xC9); /* mov [r9 + rcx*8], rdx */
    b8(o, 0xFF), b8(o, 0xC1);                          /* inc ecx */
    b8(o, 0xE9);                                       /* jmp loop */
    jmp = snx_xelf_put(o, SNX_ELF_TEXT, NULL, 4, 1);
    done = snx_xelf_put(o, SNX_ELF_TEXT, NULL, 0, 1);
    lea(o, 0, "MOD");
    b8(o, 0xC3);
    {
        unsigned char* t = snx_xelf_bytes(o, SNX_ELF_TEXT);
        SnxU32 d1 = (SnxU32)(done - (jae + 4)), d2 = (SnxU32)(loop - (jmp + 4));
        memcpy(t + jae, &d1, 4);
        memcpy(t + jmp, &d2, 4);
    }
}

/* snx_import_dyn(c, e): the function dispatch has for e (CPU.disp, inside [code_lo, code_hi) and aligned), else the
   runtime's miss, tail called with c and e: snx_lookup's, as emit_imports' CALLDYN */
static void import_dyn(SnxXElf* o) {
    size_t miss, found, at;
    fn_begin(o, "snx_import_dyn", SNX_ELF_LOCAL);
    b8(o, 0x48), b8(o, 0x89), b8(o, 0xF0);                                   /* mov rax, rsi */
    b8(o, 0x48), b8(o, 0x2B), b8(o, 0x87), b32(o, (SnxU32)offsetof(CPU, code_lo)); /* sub rax, [rdi + lo] */
    b8(o, 0x48), b8(o, 0x8B), b8(o, 0x97), b32(o, (SnxU32)offsetof(CPU, code_hi)); /* mov rdx, [rdi + hi] */
    b8(o, 0x48), b8(o, 0x2B), b8(o, 0x97), b32(o, (SnxU32)offsetof(CPU, code_lo)); /* sub rdx, [rdi + lo] */
    b8(o, 0x48), b8(o, 0x39), b8(o, 0xD0);                                   /* cmp rax, rdx */
    b8(o, 0x0F), b8(o, 0x83);                                                /* jae miss */
    miss = snx_xelf_put(o, SNX_ELF_TEXT, NULL, 4, 1);
    b8(o, 0x40), b8(o, 0xF6), b8(o, 0xC6), b8(o, 0x03);                      /* test sil, 3 */
    b8(o, 0x0F), b8(o, 0x85);                                                /* jnz miss */
    at = snx_xelf_put(o, SNX_ELF_TEXT, NULL, 4, 1);
    b8(o, 0x48), b8(o, 0xC1), b8(o, 0xE8), b8(o, 0x02);                      /* shr rax, 2 */
    b8(o, 0x48), b8(o, 0x8B), b8(o, 0x97), b32(o, (SnxU32)offsetof(CPU, disp)); /* mov rdx, [rdi + disp] */
    b8(o, 0x48), b8(o, 0x8B), b8(o, 0x04), b8(o, 0xC2);                      /* mov rax, [rdx + rax*8] */
    b8(o, 0x48), b8(o, 0x85), b8(o, 0xC0);                                   /* test rax, rax */
    b8(o, 0x0F), b8(o, 0x85);                                                /* jnz found */
    found = snx_xelf_put(o, SNX_ELF_TEXT, NULL, 4, 1);
    {
        size_t here = snx_xelf_put(o, SNX_ELF_TEXT, NULL, 0, 1);
        unsigned char* t = snx_xelf_bytes(o, SNX_ELF_TEXT);
        SnxU32 d = (SnxU32)(here - (miss + 4));
        memcpy(t + miss, &d, 4);
        d = (SnxU32)(here - (at + 4));
        memcpy(t + at, &d, 4);
    }
    b8(o, 0x48), b8(o, 0x8B), b8(o, 0x87), b32(o, (SnxU32)offsetof(CPU, rt));    /* mov rax, [rdi + rt] */
    b8(o, 0x48), b8(o, 0x8B), b8(o, 0x80), b32(o, (SnxU32)offsetof(RtAPI, miss)); /* mov rax, [rax + miss] */
    {
        size_t here = snx_xelf_put(o, SNX_ELF_TEXT, NULL, 0, 1);
        unsigned char* t = snx_xelf_bytes(o, SNX_ELF_TEXT);
        SnxU32 d = (SnxU32)(here - (found + 4));
        memcpy(t + found, &d, 4);
    }
    b8(o, 0xFF), b8(o, 0xE0); /* jmp rax */
}

int snx_x64_descriptor(const SnxCodegen* codegen, const SnxCodegenOptions* options, const SnxProgram* loaded,
                       const SnxU64* imports, size_t import_count, unsigned char** object, size_t* size,
                       size_t* entry_count, char* error, size_t error_size) {
    SnxXElf* o = snx_xelf_new();
    SnxModuleEntry* entries;
    size_t count, i, nfns = codegen->function_count ? codegen->function_count : 1, at;
    SnxU64 lo = ~(SnxU64)0, hi = 0;
    char name[48];
    int rc;
    if (!o) {
        snprintf(error, error_size, "out of memory");
        return -1;
    }
    entries = snx_module_entries(codegen, &count);
    for (i = 0; i < count; ++i)
        if (entries[i].address < SNX_CODE_BASE || entries[i].address - SNX_CODE_BASE > 0xFFFFFFFFUL) {
            free(entries);
            snx_xelf_free(o);
            snprintf(error, error_size, "AOT entry exceeds the module ABI's 32-bit offset range");
            return -1;
        }
    /* FNS[]: every function; FN_LO[], FN_LEN[]: the guest code each was made from */
    at = snx_xelf_put(o, SNX_ELF_RELRO, NULL, 8 * nfns, 8);
    snx_xelf_symbol(o, "FNS", SNX_ELF_RELRO, at, 8 * nfns, SNX_ELF_LOCAL);
    for (i = 0; i < codegen->function_count; ++i) {
        fn_name(name, sizeof name, codegen->functions[i].start);
        ptr(o, SNX_ELF_RELRO, at + 8 * i, name);
    }
    {
        SnxU64* fn_lo = (SnxU64*)calloc(nfns, sizeof *fn_lo);
        SnxU32* fn_len = (SnxU32*)calloc(nfns, sizeof *fn_len);
        for (i = 0; i < codegen->function_count; ++i) {
            const SnxFunction* f = &codegen->functions[i];
            SnxU64 first = f->count ? snx_function_address(f, 0) : 0;
            SnxU64 end = f->count ? snx_function_address(f, f->count - 1) + 4 : 0;
            if (end - first > 0xFFFFFFFFu) {
                free(fn_lo), free(fn_len), free(entries);
                snx_xelf_free(o);
                snprintf(error, error_size, "a function's code spans more than the module ABI's 32-bit length");
                return -1;
            }
            fn_lo[i] = first;
            fn_len[i] = (SnxU32)(end - first);
        }
        at = snx_xelf_put(o, SNX_ELF_RODATA, fn_lo, 8 * nfns, 8);
        snx_xelf_symbol(o, "FN_LO", SNX_ELF_RODATA, at, 8 * nfns, SNX_ELF_LOCAL);
        at = snx_xelf_put(o, SNX_ELF_RODATA, fn_len, 4 * nfns, 4);
        snx_xelf_symbol(o, "FN_LEN", SNX_ELF_RODATA, at, 4 * nfns, SNX_ELF_LOCAL);
        free(fn_lo), free(fn_len);
    }
    /* snx_stale[]: each function's byte, the runtime's to set */
    at = snx_xelf_put(o, SNX_ELF_BSS, NULL, nfns, 8);
    snx_xelf_symbol(o, "snx_stale", SNX_ELF_BSS, at, nfns, SNX_ELF_GLOBAL | SNX_ELF_HIDDEN);
    /* ENT_OFF[], ENT_FN[], and ENT_ADDR[], ENT_PTR[] snx_module() fills */
    {
        size_t n = count ? count : 1;
        SnxU32* off = (SnxU32*)calloc(n, 4);
        SnxU32* fn = (SnxU32*)calloc(n, 4);
        for (i = 0; i < count; ++i) {
            off[i] = (SnxU32)(entries[i].address - SNX_CODE_BASE);
            fn[i] = (SnxU32)entries[i].function;
        }
        at = snx_xelf_put(o, SNX_ELF_RODATA, off, 4 * n, 4);
        snx_xelf_symbol(o, "ENT_OFF", SNX_ELF_RODATA, at, 4 * n, SNX_ELF_LOCAL);
        at = snx_xelf_put(o, SNX_ELF_RODATA, fn, 4 * n, 4);
        snx_xelf_symbol(o, "ENT_FN", SNX_ELF_RODATA, at, 4 * n, SNX_ELF_LOCAL);
        free(off), free(fn);
        at = snx_xelf_put(o, SNX_ELF_BSS, NULL, 8 * n, 8);
        snx_xelf_symbol(o, "ENT_ADDR", SNX_ELF_BSS, at, 8 * n, SNX_ELF_LOCAL);
        at = snx_xelf_put(o, SNX_ELF_BSS, NULL, 8 * n, 8);
        snx_xelf_symbol(o, "ENT_PTR", SNX_ELF_BSS, at, 8 * n, SNX_ELF_LOCAL);
    }
    /* MODS[]: the modules */
    {
        SnxModInfo* mods = (SnxModInfo*)calloc(loaded->module_count ? loaded->module_count : 1, sizeof *mods);
        for (i = 0; i < loaded->module_count; ++i) {
            const SnxModuleImage* m = &loaded->modules[i];
            mods[i].base = m->base;
            mods[i].size = m->size;
            memcpy(mods[i].build_id, m->build_id, 32);
            mods[i].text_off = m->text_off;
            mods[i].text_size = m->text_size;
            mods[i].ro_off = m->ro_off;
            mods[i].ro_size = m->ro_size;
            mods[i].data_off = m->data_off;
            mods[i].data_size = m->data_size;
            mods[i].bss_size = m->bss;
            mods[i].is_nro = !!m->is_nro;
            if (m->base + m->text_off < lo) lo = m->base + m->text_off;
            if (m->base + m->text_off + m->text_size > hi) hi = m->base + m->text_off + m->text_size;
        }
        at = snx_xelf_put(o, SNX_ELF_RELRO, mods, loaded->module_count * sizeof *mods, 8);
        snx_xelf_symbol(o, "MODS", SNX_ELF_RELRO, at, loaded->module_count * sizeof *mods, SNX_ELF_LOCAL);
        for (i = 0; i < loaded->module_count; ++i) {
            snprintf(name, sizeof name, "MOD_NAME_%lu", (unsigned long)i);
            string(o, loaded->modules[i].name, name);
            ptr(o, SNX_ELF_RELRO, at + i * sizeof *mods + offsetof(SnxModInfo, name), name);
        }
        free(mods);
    }
    /* MOD: the descriptor */
    {
        SnxModule mod;
        const SnxNpdm* npdm = &loaded->npdm;
        memset(&mod, 0, sizeof mod);
        mod.abi = options->abi;
        mod.nmods = (u32)loaded->module_count;
        mod.nentries = (u32)count;
        mod.code_lo = lo;
        mod.code_hi = hi;
        mod.title_id = npdm->title_id;
        mod.main_stack = (u32)npdm->stack;
        mod.main_prio = (u32)npdm->prio;
        mod.main_core = (u32)npdm->core;
        mod.nfns = (u32)codegen->function_count;
        string(o, options->title ? options->title : "Game", "TITLE");
        at = snx_xelf_put(o, SNX_ELF_DATA, &mod, sizeof mod, 8);
        snx_xelf_symbol(o, "MOD", SNX_ELF_DATA, at, sizeof mod, SNX_ELF_LOCAL);
        ptr(o, SNX_ELF_DATA, at + offsetof(SnxModule, mods), "MODS");
        ptr(o, SNX_ELF_DATA, at + offsetof(SnxModule, entry_addr), "ENT_ADDR");
        ptr(o, SNX_ELF_DATA, at + offsetof(SnxModule, entry_fn), "ENT_PTR");
        ptr(o, SNX_ELF_DATA, at + offsetof(SnxModule, title), "TITLE");
        ptr(o, SNX_ELF_DATA, at + offsetof(SnxModule, fn), "FNS");
        ptr(o, SNX_ELF_DATA, at + offsetof(SnxModule, fn_lo), "FN_LO");
        ptr(o, SNX_ELF_DATA, at + offsetof(SnxModule, fn_len), "FN_LEN");
        ptr(o, SNX_ELF_DATA, at + offsetof(SnxModule, stale), "snx_stale");
    }
    module_fn(o, count);
    /* snx_routes(): the route list's hashes, sorted */
    {
        size_t n = snx_hook_route_count();
        SnxU64* hashes = (SnxU64*)calloc(n ? n : 1, sizeof *hashes);
        SnxRoutes routes;
        for (i = 0; i < n; ++i) hashes[i] = snx_hook_route_hash(snx_hook_route_name(i));
        qsort(hashes, n, sizeof *hashes, snx_compare_u64);
        at = snx_xelf_put(o, SNX_ELF_RODATA, hashes, 8 * n, 8);
        snx_xelf_symbol(o, "ROUTE_NAMES", SNX_ELF_RODATA, at, 8 * n, SNX_ELF_LOCAL);
        free(hashes);
        memset(&routes, 0, sizeof routes);
        routes.version = SNX_ROUTES_VERSION;
        routes.count = (u32)n;
        routes.list = snx_hook_route_list_hash();
        routes.all = options->hook_direct_calls != 0;
        at = snx_xelf_put(o, SNX_ELF_RELRO, &routes, sizeof routes, 8);
        snx_xelf_symbol(o, "ROUTES", SNX_ELF_RELRO, at, sizeof routes, SNX_ELF_LOCAL);
        ptr(o, SNX_ELF_RELRO, at + offsetof(SnxRoutes, names), "ROUTE_NAMES");
        returns(o, "snx_routes", "ROUTES");
    }
    /* snx_nros(): where the NROs the program maps go */
    if (options->nro_count) {
        SnxNros nros;
        SnxU64 end = 0;
        for (i = 0; i < loaded->module_count; ++i)
            if (loaded->modules[i].base + loaded->modules[i].size > end)
                end = loaded->modules[i].base + loaded->modules[i].size;
        at = snx_xelf_put(o, SNX_ELF_RODATA, options->nros, options->nro_count * sizeof(SnxNroSlot), 8);
        snx_xelf_symbol(o, "NRO_SLOTS", SNX_ELF_RODATA, at, options->nro_count * sizeof(SnxNroSlot), SNX_ELF_LOCAL);
        memset(&nros, 0, sizeof nros);
        nros.version = SNX_NROS_VERSION;
        nros.count = (u32)options->nro_count;
        nros.lo = end;
        nros.hi = options->nro_end;
        at = snx_xelf_put(o, SNX_ELF_RELRO, &nros, sizeof nros, 8);
        snx_xelf_symbol(o, "NROS", SNX_ELF_RELRO, at, sizeof nros, SNX_ELF_LOCAL);
        ptr(o, SNX_ELF_RELRO, at + offsetof(SnxNros, slots), "NRO_SLOTS");
        returns(o, "snx_nros", "NROS");
    }
    /* snx_imports(): the other modules' functions the code calls, each through its pointer */
    if (import_count) {
        SnxNroImports table;
        import_dyn(o);
        at = snx_xelf_put(o, SNX_ELF_DATA, NULL, 8 * import_count, 8);
        snx_xelf_symbol(o, "IMPORT_FN", SNX_ELF_DATA, at, 8 * import_count, SNX_ELF_LOCAL);
        for (i = 0; i < import_count; ++i) ptr(o, SNX_ELF_DATA, at + 8 * i, "snx_import_dyn");
        at = snx_xelf_put(o, SNX_ELF_RODATA, imports, 8 * import_count, 8);
        snx_xelf_symbol(o, "IMPORT_ADDR", SNX_ELF_RODATA, at, 8 * import_count, SNX_ELF_LOCAL);
        for (i = 0; i < import_count; ++i) {
            /* snx_aot_<address>: jmp [IMPORT_FN + 8i] (the pointer the runtime may change, read once) */
            size_t r;
            snprintf(name, sizeof name, "snx_aot_%llx", (unsigned long long)imports[i]);
            fn_begin(o, name, SNX_ELF_GLOBAL | SNX_ELF_HIDDEN);
            b8(o, 0xFF), b8(o, 0x25);
            r = snx_xelf_put(o, SNX_ELF_TEXT, NULL, 4, 1);
            snx_xelf_reloc(o, SNX_ELF_TEXT, r, "IMPORT_FN", R_X86_64_PC32, (long long)(8 * i) - 4);
        }
        memset(&table, 0, sizeof table);
        table.version = SNX_NRO_IMPORTS_VERSION;
        table.count = (u32)import_count;
        at = snx_xelf_put(o, SNX_ELF_RELRO, &table, sizeof table, 8);
        snx_xelf_symbol(o, "IMPORTS", SNX_ELF_RELRO, at, sizeof table, SNX_ELF_LOCAL);
        ptr(o, SNX_ELF_RELRO, at + offsetof(SnxNroImports, address), "IMPORT_ADDR");
        ptr(o, SNX_ELF_RELRO, at + offsetof(SnxNroImports, function), "IMPORT_FN");
        ptr(o, SNX_ELF_RELRO, at + offsetof(SnxNroImports, dynamic), "snx_import_dyn");
        returns(o, "snx_imports", "IMPORTS");
    }
    free(entries);
    *entry_count = count;
    rc = snx_xelf_write(o, object, size);
    snx_xelf_free(o);
    if (rc) snprintf(error, error_size, "out of memory");
    return rc;
}

/* a 32-bit program's SnxArmModule, as module_arm.c's emit_descriptor gives it: ENT_ADDR the blocks' keys, ENT_FN
   their functions (A_ or T_ and the address), MODS, the title, the thread's settings and snx_arm_poll_offset, the
   module's i64 the runtime writes (hidden: the blocks read it PC-relative), and snx_module_arm() returning &MOD */
int snx_x64_arm_descriptor(const SnxProgram* loaded, const SnxCodegenOptions* options, const SnxU32* keys, size_t count,
                           unsigned char** object, size_t* size, char* error, size_t error_size) {
    SnxXElf* o = snx_xelf_new();
    size_t at, n = count ? count : 1;
    char name[48];
    if (!o) {
        snprintf(error, error_size, "out of memory");
        return -1;
    }
    {
        u32* addr = (u32*)calloc(n, sizeof *addr);
        if (!addr) {
            snx_xelf_free(o);
            snprintf(error, error_size, "out of memory");
            return -1;
        }
        for (size_t i = 0; i < count; ++i) addr[i] = keys[i];
        at = snx_xelf_put(o, SNX_ELF_RODATA, addr, n * sizeof *addr, 4);
        snx_xelf_symbol(o, "ENT_ADDR", SNX_ELF_RODATA, at, n * sizeof *addr, SNX_ELF_LOCAL);
        free(addr);
    }
    at = snx_xelf_put(o, SNX_ELF_RELRO, NULL, 8 * n, 8);
    snx_xelf_symbol(o, "ENT_FN", SNX_ELF_RELRO, at, 8 * n, SNX_ELF_LOCAL);
    for (size_t i = 0; i < count; ++i) {
        snprintf(name, sizeof name, "%c_%08x", keys[i] & 1 ? 'T' : 'A', (unsigned)(keys[i] & ~1u));
        ptr(o, SNX_ELF_RELRO, at + 8 * i, name);
    }
    {
        SnxModInfo* mods = (SnxModInfo*)calloc(loaded->module_count ? loaded->module_count : 1, sizeof *mods);
        if (!mods) {
            snx_xelf_free(o);
            snprintf(error, error_size, "out of memory");
            return -1;
        }
        for (size_t i = 0; i < loaded->module_count; ++i) {
            const SnxModuleImage* m = &loaded->modules[i];
            mods[i].base = m->base;
            mods[i].size = m->size;
            memcpy(mods[i].build_id, m->build_id, 32);
            mods[i].text_off = m->text_off;
            mods[i].text_size = m->text_size;
            mods[i].ro_off = m->ro_off;
            mods[i].ro_size = m->ro_size;
            mods[i].data_off = m->data_off;
            mods[i].data_size = m->data_size;
            mods[i].bss_size = m->bss;
            mods[i].is_nro = !!m->is_nro;
        }
        at = snx_xelf_put(o, SNX_ELF_RELRO, mods, loaded->module_count * sizeof *mods, 8);
        snx_xelf_symbol(o, "MODS", SNX_ELF_RELRO, at, loaded->module_count * sizeof *mods, SNX_ELF_LOCAL);
        for (size_t i = 0; i < loaded->module_count; ++i) {
            snprintf(name, sizeof name, "MOD_NAME_%zu", i);
            string(o, loaded->modules[i].name, name);
            ptr(o, SNX_ELF_RELRO, at + i * sizeof *mods + offsetof(SnxModInfo, name), name);
        }
        free(mods);
    }
    {
        i64 zero = 0;
        at = snx_xelf_put(o, SNX_ELF_DATA, &zero, sizeof zero, 8);
        snx_xelf_symbol(o, "snx_arm_poll_offset", SNX_ELF_DATA, at, sizeof zero, SNX_ELF_GLOBAL | SNX_ELF_HIDDEN);
    }
    string(o, options->title ? options->title : "Game", "TITLE");
    {
        SnxArmModule mod = {0};
        const SnxNpdm* npdm = &loaded->npdm;
        mod.abi = SNX_ARM_MODULE_ABI;
        mod.nmods = (u32)loaded->module_count;
        mod.nentries = (u32)count;
        mod.title_id = npdm->title_id;
        mod.main_stack = (u32)npdm->stack;
        mod.main_prio = npdm->prio;
        mod.main_core = npdm->core;
        mod.aspace = npdm->aspace;
        at = snx_xelf_put(o, SNX_ELF_RELRO, &mod, sizeof mod, 8);
        snx_xelf_symbol(o, "MOD", SNX_ELF_RELRO, at, sizeof mod, SNX_ELF_LOCAL);
        ptr(o, SNX_ELF_RELRO, at + offsetof(SnxArmModule, mods), "MODS");
        ptr(o, SNX_ELF_RELRO, at + offsetof(SnxArmModule, entry_addr), "ENT_ADDR");
        ptr(o, SNX_ELF_RELRO, at + offsetof(SnxArmModule, entry_fn), "ENT_FN");
        ptr(o, SNX_ELF_RELRO, at + offsetof(SnxArmModule, title), "TITLE");
        ptr(o, SNX_ELF_RELRO, at + offsetof(SnxArmModule, poll_offset), "snx_arm_poll_offset");
    }
    returns(o, "snx_module_arm", "MOD");
    int rc = snx_xelf_write(o, object, size);
    snx_xelf_free(o);
    if (rc) snprintf(error, error_size, "out of memory");
    return rc;
}

#else
int snx_x64_arm_descriptor(const SnxProgram* loaded, const SnxCodegenOptions* options, const SnxU32* keys, size_t count,
                           unsigned char** object, size_t* size, char* error, size_t error_size) {
    (void)loaded, (void)options, (void)keys, (void)count, (void)object, (void)size;
    snprintf(error, error_size, "no x86-64 back end on this host");
    return -1;
}
int snx_x64_descriptor(const SnxCodegen* codegen, const SnxCodegenOptions* options, const SnxProgram* loaded,
                       const SnxU64* imports, size_t import_count, unsigned char** object, size_t* size,
                       size_t* entry_count, char* error, size_t error_size) {
    (void)codegen, (void)options, (void)loaded, (void)imports, (void)import_count, (void)object, (void)size;
    (void)entry_count;
    snprintf(error, error_size, "no x86-64 back end on this host");
    return -1;
}
#endif
