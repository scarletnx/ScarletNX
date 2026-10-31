/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The layout of the contract between a game module and the runtime as it was when SNX_ABI last rose: cpu.h's CPU and
   its SNX_FP_* numbers, interface.h's RtAPI and SNX_SVC_COUNT, module_abi.h's descriptor. A module of SNX_ABI reads its
   fields where they are, so a change to any of them fails here until SNX_ABI rises and the table below is recorded
   again (docs/ARCHITECTURE.md, "Module ABI"); the failure prints the table to record. Not seen: a helper's results
   (cpu_helpers.c), and a field added into a struct's tail padding, which moves nothing; those stay a rule */
#include "common/cpu/aarch64/cpu.h"
#include "common/runtime/interface.h"
#include "nx/loader/module_abi.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* a member's offset and size, a type's size or a number (size 0) */
typedef struct {
    const char* name;
    unsigned long long value, size;
} Field;

#define AT(type, field) {#type "." #field, offsetof(type, field), sizeof(((type*)0)->field)}
#define SIZE(type)      {"sizeof(" #type ")", sizeof(type), 0}
#define VALUE(name)     {#name, (unsigned long long)(name), 0}

/* the SNX_ABI the table below was recorded at, and the table */
#define RECORDED_ABI 16
static const Field g_recorded[] = {
    {"sizeof(V128)", 16, 0},
    {"sizeof(CPU)", 904, 0},
    {"CPU.x", 0, 256},
    {"CPU.pc", 256, 8},
    {"CPU.n", 264, 4},
    {"CPU.z", 268, 4},
    {"CPU.cf", 272, 4},
    {"CPU.vf", 276, 4},
    {"CPU.v", 280, 512},
    {"CPU.fpcr", 792, 4},
    {"CPU.fpsr", 796, 4},
    {"CPU.tpidr", 800, 8},
    {"CPU.tpidrro", 808, 8},
    {"CPU.excl_addr", 816, 8},
    {"CPU.excl_val", 824, 8},
    {"CPU.excl_val2", 832, 8},
    {"CPU.excl_size", 840, 4},
    {"CPU.excl_on", 844, 4},
    {"CPU.mem", 848, 8},
    {"CPU.disp", 856, 8},
    {"CPU.code_lo", 864, 8},
    {"CPU.code_hi", 872, 8},
    {"CPU.rt", 880, 8},
    {"CPU.thread", 888, 8},
    {"CPU.poll", 896, 4},
    {"SNX_FP_ADD", 0, 0},
    {"SNX_FP_SUB", 1, 0},
    {"SNX_FP_MUL", 2, 0},
    {"SNX_FP_DIV", 3, 0},
    {"SNX_FP_MULX", 4, 0},
    {"SNX_FP_ABD", 5, 0},
    {"SNX_FP_RECPS", 6, 0},
    {"SNX_FP_RSQRTS", 7, 0},
    {"SNX_FP_SQRT", 0, 0},
    {"SNX_FP_RECPE", 1, 0},
    {"SNX_FP_RSQRTE", 2, 0},
    {"SNX_FP_RECPX", 3, 0},
    {"sizeof(RtAPI)", 104, 0},
    {"RtAPI.svc", 0, 8},
    {"RtAPI.undef", 8, 8},
    {"RtAPI.note_hint", 16, 8},
    {"RtAPI.ticks", 24, 8},
    {"RtAPI.brk", 32, 8},
    {"RtAPI.fpcr_changed", 40, 8},
    {"RtAPI.sev", 48, 8},
    {"RtAPI.wfe", 56, 8},
    {"RtAPI.poll", 64, 8},
    {"RtAPI.svcs", 72, 8},
    {"RtAPI.miss", 80, 8},
    {"RtAPI.cas128", 88, 8},
    {"RtAPI.code_changed", 96, 8},
    {"SNX_SVC_COUNT", 256, 0},
    {"sizeof(SnxModInfo)", 88, 0},
    {"SnxModInfo.name", 0, 8},
    {"SnxModInfo.base", 8, 8},
    {"SnxModInfo.size", 16, 8},
    {"SnxModInfo.build_id", 24, 32},
    {"SnxModInfo.text_off", 56, 4},
    {"SnxModInfo.text_size", 60, 4},
    {"SnxModInfo.ro_off", 64, 4},
    {"SnxModInfo.ro_size", 68, 4},
    {"SnxModInfo.data_off", 72, 4},
    {"SnxModInfo.data_size", 76, 4},
    {"SnxModInfo.bss_size", 80, 4},
    {"SnxModInfo.is_nro", 84, 4},
    {"sizeof(SnxModule)", 128, 0},
    {"SnxModule.abi", 0, 4},
    {"SnxModule.nmods", 4, 4},
    {"SnxModule.mods", 8, 8},
    {"SnxModule.nentries", 16, 4},
    {"SnxModule.entry_addr", 24, 8},
    {"SnxModule.entry_fn", 32, 8},
    {"SnxModule.interp", 40, 8},
    {"SnxModule.code_lo", 48, 8},
    {"SnxModule.code_hi", 56, 8},
    {"SnxModule.title", 64, 8},
    {"SnxModule.title_id", 72, 8},
    {"SnxModule.main_stack", 80, 4},
    {"SnxModule.main_prio", 84, 4},
    {"SnxModule.main_core", 88, 4},
    {"SnxModule.nfns", 92, 4},
    {"SnxModule.fn", 96, 8},
    {"SnxModule.fn_lo", 104, 8},
    {"SnxModule.fn_len", 112, 8},
    {"SnxModule.stale", 120, 8},
};

int main(void) {
    static const Field now[] = {
        SIZE(V128),
        SIZE(CPU),
        AT(CPU, x),
        AT(CPU, pc),
        AT(CPU, n),
        AT(CPU, z),
        AT(CPU, cf),
        AT(CPU, vf),
        AT(CPU, v),
        AT(CPU, fpcr),
        AT(CPU, fpsr),
        AT(CPU, tpidr),
        AT(CPU, tpidrro),
        AT(CPU, excl_addr),
        AT(CPU, excl_val),
        AT(CPU, excl_val2),
        AT(CPU, excl_size),
        AT(CPU, excl_on),
        AT(CPU, mem),
        AT(CPU, disp),
        AT(CPU, code_lo),
        AT(CPU, code_hi),
        AT(CPU, rt),
        AT(CPU, thread),
        AT(CPU, poll),
        VALUE(SNX_FP_ADD),
        VALUE(SNX_FP_SUB),
        VALUE(SNX_FP_MUL),
        VALUE(SNX_FP_DIV),
        VALUE(SNX_FP_MULX),
        VALUE(SNX_FP_ABD),
        VALUE(SNX_FP_RECPS),
        VALUE(SNX_FP_RSQRTS),
        VALUE(SNX_FP_SQRT),
        VALUE(SNX_FP_RECPE),
        VALUE(SNX_FP_RSQRTE),
        VALUE(SNX_FP_RECPX),
        SIZE(RtAPI),
        AT(RtAPI, svc),
        AT(RtAPI, undef),
        AT(RtAPI, note_hint),
        AT(RtAPI, ticks),
        AT(RtAPI, brk),
        AT(RtAPI, fpcr_changed),
        AT(RtAPI, sev),
        AT(RtAPI, wfe),
        AT(RtAPI, poll),
        AT(RtAPI, svcs),
        AT(RtAPI, miss),
        AT(RtAPI, cas128),
        AT(RtAPI, code_changed),
        VALUE(SNX_SVC_COUNT),
        SIZE(SnxModInfo),
        AT(SnxModInfo, name),
        AT(SnxModInfo, base),
        AT(SnxModInfo, size),
        AT(SnxModInfo, build_id),
        AT(SnxModInfo, text_off),
        AT(SnxModInfo, text_size),
        AT(SnxModInfo, ro_off),
        AT(SnxModInfo, ro_size),
        AT(SnxModInfo, data_off),
        AT(SnxModInfo, data_size),
        AT(SnxModInfo, bss_size),
        AT(SnxModInfo, is_nro),
        SIZE(SnxModule),
        AT(SnxModule, abi),
        AT(SnxModule, nmods),
        AT(SnxModule, mods),
        AT(SnxModule, nentries),
        AT(SnxModule, entry_addr),
        AT(SnxModule, entry_fn),
        AT(SnxModule, interp),
        AT(SnxModule, code_lo),
        AT(SnxModule, code_hi),
        AT(SnxModule, title),
        AT(SnxModule, title_id),
        AT(SnxModule, main_stack),
        AT(SnxModule, main_prio),
        AT(SnxModule, main_core),
        AT(SnxModule, nfns),
        AT(SnxModule, fn),
        AT(SnxModule, fn_lo),
        AT(SnxModule, fn_len),
        AT(SnxModule, stale),
    };
    const size_t count = sizeof now / sizeof *now, recorded = sizeof g_recorded / sizeof *g_recorded;
    size_t i;
    int moved = 0;
    if (UINTPTR_MAX != UINT64_MAX) {
        /* the pointers after CPU.mem move with their size: the layout is a 64-bit host's, the only one games run on */
        printf("module_contract: not checked on a %u-bit host, where no game module runs\n",
               (unsigned)(sizeof(void*) * 8));
        return 0;
    }
    for (i = 0; i < count || i < recorded; ++i) {
        if (i >= count)
            fprintf(stderr, "%s, recorded at SNX_ABI %d, is gone\n", g_recorded[i].name, RECORDED_ABI);
        else if (i >= recorded || strcmp(now[i].name, g_recorded[i].name))
            fprintf(stderr, "%s is %llu, where the table recorded at SNX_ABI %d has %s\n", now[i].name, now[i].value,
                    RECORDED_ABI, i < recorded ? g_recorded[i].name : "nothing");
        else if (now[i].value != g_recorded[i].value || now[i].size != g_recorded[i].size) {
            if (now[i].size || g_recorded[i].size)
                fprintf(stderr, "%s is at %llu, %llu bytes, and at %llu, %llu bytes, at SNX_ABI %d\n", now[i].name,
                        now[i].value, now[i].size, g_recorded[i].value, g_recorded[i].size, RECORDED_ABI);
            else
                fprintf(stderr, "%s is %llu, %llu at SNX_ABI %d\n", now[i].name, now[i].value, g_recorded[i].value,
                        RECORDED_ABI);
        } else
            continue;
        moved = 1;
    }
    if (!moved && SNX_ABI == RECORDED_ABI) {
        printf("module_contract: %zu fields and numbers as recorded at SNX_ABI %d\n", count, RECORDED_ABI);
        return 0;
    }
    if (moved)
        fprintf(stderr,
                "The module contract moved: a module of SNX_ABI %d reads it as above. Raise SNX_ABI past %d (cpu.h, "
                "docs/ARCHITECTURE.md's Module ABI), then record this table here (tests/module_contract.c) with "
                "RECORDED_ABI the new SNX_ABI:\n",
                SNX_ABI, SNX_ABI);
    else
        fprintf(stderr, "SNX_ABI is %d, and the table here was recorded at %d: record it again with RECORDED_ABI %d:\n",
                SNX_ABI, RECORDED_ABI, SNX_ABI);
    for (i = 0; i < count; ++i) fprintf(stderr, "    {\"%s\", %llu, %llu},\n", now[i].name, now[i].value, now[i].size);
    return 1;
}
