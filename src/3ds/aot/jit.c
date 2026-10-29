/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The blocks the static module lacks, compiled while the game runs: an indirect branch's target the static discovery
   didn't find, or code the game loads at run time. Each is compiled from the guest's memory as the static ones are
   (the ARM11 profile, checked memory, which compares each instruction's bytes with the ones compiled when it runs) by
   the ARM backend's executor, and kept by its pc. */
#include "3ds/aot/ctr_jit.h"
#include "3ds/aot/ctr_module.h"
#include "3ds/aot/ctr_discover.h"
#include "3ds/runtime/internal.h"
#include "common/utilities/endian.h"
#include "common/cpu/arm/exec.h"
#include "common/utilities/common.h"
#include "snx_settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint64_t key; /* (pc << 1 | thumb) + 1, 0 for an empty slot */
    SnxArmBlock block;
    bool hinted; /* in the title's hints already */
} Slot;
struct SnxCtrJit {
    SnxCtrProcess* process;
    SnxArmExec* exec;
    Slot* slots; /* open addressing, a power of two of them, at most half full */
    size_t count, capacity;
    uint64_t compiled;
};
static size_t slot_of(const SnxCtrJit* j, uint64_t key) {
    size_t i = (size_t)((key * UINT64_C(0x9E3779B97F4A7C15)) >> 20) & (j->capacity - 1);
    while (j->slots[i].key && j->slots[i].key != key) i = (i + 1) & (j->capacity - 1);
    return i;
}
static bool grow(SnxCtrJit* j) {
    if (j->count + 1 <= j->capacity / 2) return true;
    size_t capacity = j->capacity ? j->capacity * 2 : 4096;
    Slot *old = j->slots, *slots = calloc(capacity, sizeof *slots);
    if (!slots) return false;
    size_t old_capacity = j->capacity;
    j->slots = slots, j->capacity = capacity;
    for (size_t i = 0; i < old_capacity; ++i)
        if (old[i].key) j->slots[slot_of(j, old[i].key)] = old[i];
    free(old);
    return true;
}
/* The native code of a dropped block may belong to the one that is running the IPC or SVC that dropped it, so the
   executor goes at the dispatcher's next block and the next compile makes a new one */
static void reclaim(void* context) {
    SnxCtrJit* j = context;
    snx_arm_exec_free(j->exec);
    j->exec = NULL;
    j->process->reclaim = NULL;
}
/* A CRO loaded over an address another one left would otherwise run the blocks made from the previous occupant */
static void flush(void* context) {
    SnxCtrJit* j = context;
    if (j->slots) memset(j->slots, 0, j->capacity * sizeof *j->slots);
    j->count = 0;
    j->process->reclaim = reclaim;
}
static bool ensure_exec(SnxCtrJit* j, char* error, size_t error_size) {
    if (!j->exec) j->exec = snx_arm_exec_new(error, error_size);
    return j->exec != NULL;
}
static SnxArmBlock compile(void* context, uint32_t pc, bool thumb, bool stale) {
    SnxCtrJit* j = context;
    char error[512];
    if (!ensure_exec(j, error, sizeof error)) {
        fprintf(stderr, "3ds jit: cannot create the executor: %s\n", error);
        return NULL;
    }
    uint64_t key = ((uint64_t)pc << 1 | thumb) + 1;
    if (!grow(j)) return NULL;
    size_t slot = slot_of(j, key);
    if (j->slots[slot].key && !stale) return j->slots[slot].block;
    /* as many instructions as the static blocks take, up to the first the game can't execute */
    unsigned width = thumb ? 2 : 4, count = 0;
    uint32_t words[SNX_CTR_AOT_BLOCK_WORDS];
    uint16_t halves[SNX_CTR_AOT_BLOCK_WORDS];
    for (; count < SNX_CTR_AOT_BLOCK_WORDS; ++count) {
        unsigned char b[4];
        if (snx_ctr_fetch(j->process->space, pc + count * width, b, width) != SNX_CTR_MEM_OK) break;
        words[count] = thumb ? snx_ld_le16(b) : snx_ld_le32(b);
        halves[count] = (uint16_t)words[count];
    }
    if (!count) return NULL;
    size_t consumed = 0;
    SnxArmBlock block = snx_arm_exec_compile(j->exec, thumb ? (const void*)halves : (const void*)words, count, pc,
                                             thumb, SNX_ARM_PROFILE_ARM11,
                                             snx_ctr_module_memory() == SNX_CTR_MODULE_CHECKED_MODE, &consumed);
    if (!block) {
        fprintf(stderr, "3ds jit: cannot compile the %s block at 0x%08x\n", thumb ? "Thumb" : "ARM", pc);
        return NULL;
    }
    if (!j->slots[slot].key) ++j->count;
    j->slots[slot] = (Slot){key, block, false};
    ++j->compiled;
    if (g_settings.ctr_run_time_compile_trace) fprintf(stderr, "3ds jit: compiled %08x\n", pc | thumb);
    return block;
}
SnxCtrJit* snx_ctr_jit_new(SnxCtrProcess* process, char* error, size_t error_size) {
    SnxCtrJit* j = calloc(1, sizeof *j);
    if (!j) {
        snprintf(error, error_size, "out of memory");
        return NULL;
    }
    j->process = process;
    if (!ensure_exec(j, error, error_size)) {
        free(j);
        return NULL;
    }
    process->compile = compile;
    process->compiler = j;
    process->flush = flush;
    process->reclaim = NULL;
    return j;
}
size_t snx_ctr_jit_save_hints(SnxCtrJit* j, const char* directory) {
    char* path = j && j->count ? snx_ctr_game_file(directory, SNX_CTR_HINTS) : NULL;
    if (!path) return 0;
    FILE* f = fopen(path, "r");
    unsigned value;
    while (f && fscanf(f, "%x", &value) == 1) {
        uint64_t key = ((uint64_t)(value & ~1u) << 1 | (value & 1)) + 1;
        size_t slot = slot_of(j, key);
        if (j->slots[slot].key) j->slots[slot].hinted = true;
    }
    if (f) fclose(f);
    size_t added = 0;
    f = fopen(path, "a");
    for (size_t i = 0; f && i < j->capacity; ++i) {
        Slot* s = &j->slots[i];
        if (!s->key || s->hinted) continue;
        uint64_t key = s->key - 1;
        if (fprintf(f, "%08x\n", (unsigned)(key >> 1) | (unsigned)(key & 1)) < 0) break;
        s->hinted = true;
        ++added;
    }
    if (f && fclose(f)) added = 0;
    free(path);
    return added;
}
uint64_t snx_ctr_jit_compiled(const SnxCtrJit* j) { return j ? j->compiled : 0; }
void snx_ctr_jit_free(SnxCtrJit* j) {
    if (!j) return;
    if (j->process->compiler == j) {
        j->process->compile = NULL;
        j->process->compiler = NULL;
        j->process->flush = NULL;
        j->process->reclaim = NULL;
    }
    snx_arm_exec_free(j->exec);
    free(j->slots);
    free(j);
}
