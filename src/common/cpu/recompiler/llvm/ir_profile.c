/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/cpu/recompiler/llvm/ir.h"
#include "common/utilities/common.h"
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Recorded branch counts, not guesses. Guest PC + instruction word is the
   identity: a recompiled or patched instruction must not inherit stale odds. */
typedef struct {
    SnxU64 pc;
    SnxU32 word;
    SnxU32 taken, fall;
} BranchCount;

typedef struct {
    size_t count;
    BranchCount sites[];
} BranchProfile;

static struct {
    _Atomic(BranchProfile*) published;
    atomic_flag initializing;
    _Atomic int ready;
    char path[1024]; /* snx_ir_branch_profile's, "" for none */
} g_profile = {.initializing = ATOMIC_FLAG_INIT};

void snx_ir_branch_profile(const char* path) {
    free(atomic_exchange_explicit(&g_profile.published, NULL, memory_order_relaxed));
    atomic_store_explicit(&g_profile.ready, 0, memory_order_relaxed);
    atomic_flag_clear_explicit(&g_profile.initializing, memory_order_release);
    if (path && strlen(path) >= sizeof g_profile.path) {
        snx_error("ir: ir_branch_profile=%s is longer than %u bytes: no branch weights", path,
                  (unsigned)sizeof g_profile.path - 1);
        path = NULL;
    }
    snprintf(g_profile.path, sizeof g_profile.path, "%s", path ? path : "");
}

static int by_site(const void* a, const void* b) {
    const BranchCount* x = a;
    const BranchCount* y = b;
    if (x->pc != y->pc) return x->pc < y->pc ? -1 : 1;
    return (x->word > y->word) - (x->word < y->word);
}

/* The file ir_branch_profile names, said when it cannot be read and for each kind of line that gives no weights */
static BranchProfile* read_profile(void) {
    const char* path = g_profile.path;
    FILE* file;
    size_t count = 0, capacity = 64, number = 0, bad = 0, bad_at = 0;
    char bad_line[96] = {0};
    if (!*path) return NULL;
    if (!(file = fopen(path, "r"))) {
        snx_error("ir: ir_branch_profile=%s cannot be read (%s): no branch weights", path, strerror(errno));
        return NULL;
    }
    BranchProfile* profile = malloc(sizeof(*profile) + capacity * sizeof(BranchCount));
    if (!profile) {
        fclose(file);
        snx_error("ir: ir_branch_profile=%s: no memory to read it: no branch weights", path);
        return NULL;
    }
    char line[160];
    while (fgets(line, sizeof(line), file)) {
        unsigned long long pc, taken, fall;
        unsigned word;
        ++number;
        if (line[0] == '#' || line[0] == '\n') continue;
        if (sscanf(line, "%llx %x %llu %llu", &pc, &word, &taken, &fall) != 4) {
            if (!bad++) {
                bad_at = number;
                snprintf(bad_line, sizeof bad_line, "%.*s", (int)strcspn(line, "\r\n"), line);
            }
            continue;
        }
        if (!taken && !fall) continue;
        if (count == capacity) {
            BranchProfile* larger =
                capacity < (1u << 20) ? realloc(profile, sizeof(*profile) + capacity * 2 * sizeof(BranchCount)) : NULL;
            if (!larger) {
                snx_error("ir: ir_branch_profile=%s: %s %zu branches: line %zu and those after it give no weights",
                          path, capacity < (1u << 20) ? "no memory for more than" : "it reads at most", count, number);
                break;
            }
            capacity *= 2;
            profile = larger;
        }
        /* Normalize counts before converting to LLVM's 32-bit metadata. */
        while (taken > 0xFFFFFFFFULL || fall > 0xFFFFFFFFULL) {
            taken = (taken + 1) / 2;
            fall = (fall + 1) / 2;
        }
        profile->sites[count++] = (BranchCount){pc, word, (SnxU32)taken, (SnxU32)fall};
    }
    fclose(file);
    if (bad)
        snx_error("ir: ir_branch_profile=%s: %zu line%s not a branch's address and word in hex, then how many times it "
                  "was taken and not taken (line %zu: \"%s\"): no weights from %s",
                  path, bad, bad > 1 ? "s are" : " is", bad_at, bad_line, bad > 1 ? "them" : "it");
    qsort(profile->sites, count, sizeof(BranchCount), by_site);
    /* Keep one representative per identity. qsort does not preserve the input order of equal identities,
       so this does not promise which repeated entry supplies its counts. */
    size_t out = 0;
    for (size_t i = 0; i < count; ++i) {
        if ((i && by_site(&profile->sites[i], &profile->sites[i - 1]) == 0)) continue;
        profile->sites[out++] = profile->sites[i];
    }
    profile->count = out;
    return profile;
}

static const BranchProfile* branch_profile(void) {
    if (!atomic_load_explicit(&g_profile.ready, memory_order_acquire)) {
        if (!atomic_flag_test_and_set_explicit(&g_profile.initializing, memory_order_acquire)) {
            BranchProfile* data = read_profile();
            atomic_store_explicit(&g_profile.published, data, memory_order_release);
            atomic_store_explicit(&g_profile.ready, 1, memory_order_release);
        } else {
            while (!atomic_load_explicit(&g_profile.ready, memory_order_acquire)) {}
        }
    }
    return atomic_load_explicit(&g_profile.published, memory_order_acquire);
}

void ir_profile_branch(SnxIr* ir, LLVMValueRef branch, SnxU64 pc, SnxU32 word) {
    const BranchProfile* profile = branch_profile();
    if (!profile) return;
    size_t lo = 0, hi = profile->count;
    BranchCount key = {.pc = pc, .word = word};
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (by_site(&profile->sites[mid], &key) < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo == profile->count || by_site(&profile->sites[lo], &key)) return;
    const BranchCount* sample = &profile->sites[lo];
    LLVMMetadataRef weights[] = {LLVMMDStringInContext2(ir->ctx, "branch_weights", 14),
                                 LLVMValueAsMetadata(ir_i32(ir, sample->taken ? sample->taken : 1)),
                                 LLVMValueAsMetadata(ir_i32(ir, sample->fall ? sample->fall : 1))};
    LLVMSetMetadata(branch, LLVMGetMDKindIDInContext(ir->ctx, "prof", 4),
                    LLVMMetadataAsValue(ir->ctx, LLVMMDNodeInContext2(ir->ctx, weights, 3)));
}
