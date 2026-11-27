/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What the sail tests hold the words QEMU can't stand for to (sail_qemu.h's frozen parts): LLVM's steps
   (llvm_steps.c) of each, from the states the tests run them in, their ends' hashes folded a word, as
   tests/data/sail_frozen.txt keeps them. Without --record it checks that LLVM's steps still give the file, which a
   change to the words, their states or LLVM's code of them breaks.
     snx-test-sail-frozen FILE
     snx-test-sail-frozen --record FILE [--recorded TEXT] */
#include "sail_qemu.h"
#include "llvm_steps.h"
#include "support/frozen.h"
#include "common/cpu/aarch64/a64_decoder.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef u64 (*Step)(CPU* c);
/* a step is generated code, with no type signature before its entry, which the function sanitizer's check of an
   indirect call reads: the call is unchecked */
[[clang::no_sanitize("function")]] static u64 run_step(Step step, CPU* c) { return step(c); }

typedef struct {
    LlvmSteps steps;
    const u32* words;
    const size_t* index; /* each word's step */
} Llvm;

static int has_step(void* user, size_t i, unsigned form) {
    (void)user, (void)i, (void)form;
    return 1;
}
static void run_llvm(void* user, size_t i, unsigned form, unsigned s, const CPU* start, u8* work, const u8* initial,
                     SailEnd* e) {
    const Llvm* l = (const Llvm*)user;
    CPU c = *start;
    u64 next;
    (void)form, (void)s;
    sail_window_start(work, initial, l->words[i], 0);
    c.mem = work - SAIL_WINDOW;
    memset(&g_sail_calls, 0, sizeof g_sail_calls);
    next = run_step((Step)l->steps.steps[l->index[i]], &c);
    sail_native_end(e, &c, next, SAIL_PC, work, initial);
}

int main(int argc, char** argv) {
    static const char* const forms[1] = {"LLVM's"};
    const char *path = NULL, *recorded = "LLVM's steps (snx-test-sail-frozen)";
    int record = 0, a;
    u32* candidates = NULL;
    size_t ncand, i, n = 0, *index, failures = 0, items[SAIL_FROZEN_PARTS] = {0}, at[SAIL_FROZEN_PARTS] = {0};
    u32* words;
    u8* initial;
    char error[1024];
    Llvm llvm = {0};
    SailHold h = {0};
    SnxFrozen f = {0};
    for (a = 1; a < argc; ++a) {
        if (!strcmp(argv[a], "--record"))
            record = 1;
        else if (!strcmp(argv[a], "--recorded") && a + 1 < argc)
            recorded = argv[++a];
        else if (!path && argv[a][0] != '-')
            path = argv[a];
        else {
            fprintf(stderr,
                    "snx-test-sail-frozen: '%s' is not an option\n"
                    "usage: snx-test-sail-frozen [--record [--recorded TEXT]] FILE\n",
                    argv[a]);
            return 2;
        }
    }
    if (!path) {
        fprintf(stderr, "usage: snx-test-sail-frozen [--record [--recorded TEXT]] FILE\n");
        return 2;
    }
    sail_set_target(SNX_A64_TARGET_OUNCE);
    ncand = sail_candidates(3, &candidates);
    words = (u32*)malloc(sizeof *words * (ncand ? ncand : 1));
    index = (size_t*)malloc(sizeof *index * (ncand ? ncand : 1));
    initial = (u8*)malloc(SAIL_WINDOW_SIZE);
    h.work = (u8*)malloc(SAIL_WINDOW_SIZE);
    if (!words || !index || !initial || !h.work) return 1;
    if (llvm_steps_begin(&llvm.steps, 0, LLVM_STEPS_HOST, error, sizeof error)) {
        fprintf(stderr, "FAIL: %s\n", error);
        return 1;
    }
    for (i = 0; i < ncand; ++i) {
        const int part = sail_frozen_part(candidates[i]);
        if (part < 0) continue;
        ++items[part];
        words[n] = candidates[i];
        index[n++] = llvm_steps_add(&llvm.steps, candidates[i], SAIL_PC, SNX_A64_TARGET_OUNCE);
    }
    if (llvm_steps_load(&llvm.steps, error, sizeof error)) {
        fprintf(stderr, "FAIL: %s\n", error);
        return 1;
    }
    llvm.words = words;
    llvm.index = index;
    h.name = "sail_frozen";
    h.forms = forms;
    h.nforms = 1;
    h.user = &llvm;
    h.has = has_step;
    h.run = run_llvm;
    sail_window(initial);
    memcpy(h.work, initial, SAIL_WINDOW_SIZE);
    if (record) {
        snprintf(f.what, sizeof f.what, "LLVM's ends of the sail tests' words QEMU can't stand for, a hash a word");
        f.generator = SAIL_FROZEN_GENERATOR;
        snprintf(f.args, sizeof f.args, "--samples 3 --states %u", SAIL_STATES_MAX);
        snprintf(f.recorded, sizeof f.recorded, "%s", recorded);
        f.inputs = sail_frozen_inputs(candidates, ncand);
        f.parts = SAIL_FROZEN_PARTS;
        f.items[0] = items[0];
        f.items[1] = items[1];
        f.hashes = (uint64_t*)malloc(sizeof *f.hashes * (n ? n : 1));
        if (!f.hashes) return 1;
    } else if (snx_frozen_read(&f, path, SAIL_FROZEN_GENERATOR, error, sizeof error)) {
        fprintf(stderr, "FAIL: %s\n", error);
        return 1;
    } else if (f.inputs != sail_frozen_inputs(candidates, ncand) || f.parts != SAIL_FROZEN_PARTS ||
               f.items[0] != items[0] || f.items[1] != items[1]) {
        fprintf(stderr,
                "FAIL: %s holds %zu and %zu words, inputs %016llx; the candidates give %zu and %zu, inputs "
                "%016llx: record it again (--record)\n",
                path, f.items[0], f.parts > 1 ? f.items[1] : 0, (unsigned long long)f.inputs, items[0], items[1],
                (unsigned long long)sail_frozen_inputs(candidates, ncand));
        return 1;
    }
    /* the parts' words in the candidates' order: the unheld ones first in the file, then the store-exclusives */
    for (i = 0; i < n; ++i) {
        const int part = sail_frozen_part(words[i]);
        const size_t item = at[part]++, slot = (part ? items[0] : 0) + item;
        const u64 got = sail_frozen_word(&h, i, 0, words[i], initial);
        if (record)
            f.hashes[slot] = got;
        else if (got != snx_frozen_hash(&f, (size_t)part, item)) {
            ++failures;
            fprintf(stderr, "FAIL: %s %08x: LLVM's ends hash to %016llx, %s:%zu holds %016llx\n",
                    snx_a64_op_text(snx_a64_decode_word(words[i], SNX_A64_TARGET_OUNCE, &(SnxA64Insn){0})),
                    (unsigned)words[i], (unsigned long long)got, path, snx_frozen_line(&f, (size_t)part, item),
                    (unsigned long long)snx_frozen_hash(&f, (size_t)part, item));
        }
    }
    if (record && snx_frozen_write(&f, path, error, sizeof error)) {
        fprintf(stderr, "FAIL: %s\n", error);
        return 1;
    }
    printf("sail_frozen: %zu words (%zu QEMU can't stand for, %zu store-exclusives), %u states each, %s %s: %zu "
           "failures\n",
           n, items[0], items[1], SAIL_STATES_MAX, record ? "recorded in" : "held to", path, failures);
    llvm_steps_end(&llvm.steps);
    snx_frozen_free(&f);
    free(candidates), free(words), free(index), free(initial), free(h.work);
    return failures != 0;
}
