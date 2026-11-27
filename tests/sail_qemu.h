/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What sail_x64.c and sail_mach.c hold a native back end's words to: qemu-aarch64 -cpu max (tests/qemu_ref), or a
   reference of it that the qemu-reference job recorded on a host where QEMU runs.

   Each word runs at SAIL_PC in a window of guest memory from guest address 0, seeded (SNX_QREF_SEED_RANDOM, SAIL_SEED)
   on both sides, from states drawn from the word's own seed, so that a word's states stay the same whichever other
   words a back end makes. A store-exclusive runs after the load-exclusive that arms its monitor in odd states, from
   SAIL_PAIR_PC, and alone in the others (its monitor off). The native code's end is compared with QEMU's: the
   registers, SP, NZCV, FPCR, FPSR, the vector registers, the memory it changed and how it ended (the next pc, UNDEF,
   BRK, SVC). A reference holds, for each (word, state), a hash of QEMU's end in the same terms (sail_end_hash), and
   the native end's hash must be it.

   Not compared, each counted and named in the summary: a word QEMU's runner can't host (lqd_op_name), a word that
   decodes as another op for max than for the guest the test makes it for (what max makes of it is not that guest's),
   a word of pointer authentication (QEMU signs with keys where the code runs with authentication off), FCVTN2 of its
   own Vn (QEMU 11.1.2 reads Vn after it has written Vd), and with a reference, a word it holds none of. */
#ifndef SNX_TEST_SAIL_QEMU_H
#define SNX_TEST_SAIL_QEMU_H
#include "common/cpu/aarch64/cpu.h"
#include "common/runtime/interface.h"
#include "qemu_ref/qemu_ref.h"
#include <stddef.h>
#include <stdint.h>

#define SAIL_WINDOW      0x0ull /* the window starts at guest address 0 on both sides */
#define SAIL_WINDOW_SIZE SNX_QREF_WINDOW_SIZE
#define SAIL_PC          (SAIL_WINDOW + 0x100000ull) /* a literal load's ±1 MiB stays in the window */
#define SAIL_PAIR_PC     (SAIL_WINDOW + SAIL_WINDOW_SIZE)
#define SAIL_CODE_SIZE   (2 * SNX_QREF_PAGE)
#define SAIL_SEED        0x5A11ull
/* the guest memory a load or a store of the tests reaches: the address registers point at 0x1000 to 0x1ff0, an index
   register too, which a 128-bit access shifts by 4 (to 0x1ff00), and the immediates reach less */
#define SAIL_TOUCHED     0x24000u
#define SAIL_STATES_MAX  64u /* the most states a word has, and those a reference holds */

/* how a run of the native code ended, as QEMU's runner says it */
typedef struct {
    u32 outcome; /* SNX_QREF_NEXT ... */
    u64 pc, imm;
} SailHow;

/* what the runtime's calls saw in a native run (sail_rt's functions): reset before each run */
typedef struct {
    unsigned undef, svc, brk, sev, wfe;
    u64 imm;
    u64 undef_args; /* what each UNDEF was given, the pc and the word, folded */
} SailCalls;
extern SailCalls g_sail_calls;
/* a runtime whose calls are noted (UNDEF, an SVC's and a BRK's immediates, SEV, WFE), the poll cleared, ticks a
   constant, cas128 the runtime's own */
void sail_rt(RtAPI* rt);

/* the decoder's guest the words are made for (SNX_A64_TARGET_*) */
void sail_set_target(u64 target);
/* whether word is a load, a store or an atomic (its state's address registers point into the window) */
int sail_is_ldst(u32 word);
/* state s of word: the registers, NZCV, FPCR, FPSR and the vector registers from the word's seed, its address
   registers in the window for a load or a store, pc SAIL_PC (SAIL_PAIR_PC for a pair) */
void sail_start(CPU* c, u32 word, unsigned s, const RtAPI* rt);
/* the load-exclusive that runs before word in state s (0: none, the word runs alone) */
u32 sail_pair(u32 word, unsigned s);
/* why word is not held to QEMU (NULL: it is) */
const char* sail_unheld(u32 word);

/* the window both sides start from (SAIL_WINDOW_SIZE bytes): the seed, pads where the code is */
void sail_window(u8* initial);
/* the bytes a run starts from in work: initial's touched part where the last native end left it changed, and the
   case's word at SAIL_PC (pads where the case is a pair, whose words are at SAIL_PAIR_PC) */
void sail_window_start(u8* work, const u8* initial, u32 word, int pair);
/* the end of a native run: its state, how it ended (from next and g_sail_calls), whether it changed the touched part
   of work */
typedef struct {
    SailHow how;
    SnxQrefState state;
    int wrote;
} SailEnd;
void sail_native_end(SailEnd* e, const CPU* c, u64 next, u64 pc, const u8* work, const u8* initial);
/* the hash of an end, QEMU's or the native one's, as the comparison takes them: of how it ended, the registers but
   the one an MRS of a system register QEMU gives otherwise reads into, and every byte of the window that differs
   from initial (work for the native side, QEMU's changes for its) */
u64 sail_end_hash(u32 word, const SailEnd* e, const u8* initial, const u8* work, const SnxQrefBatch* b,
                  const SnxQrefResult* r);
/* QEMU's end of a case's result */
void sail_qemu_end(SailEnd* e, const SnxQrefResult* r);
/* the parts of a native end that differ from QEMU's, as a short cause ("" when none), and lines that say how into
   text */
void sail_compare(u32 word, const SnxQrefState* start, const SailEnd* native, const SailEnd* qemu, const u8* initial,
                  const u8* work, const SnxQrefBatch* b, const SnxQrefResult* r, char* cause, size_t cause_size,
                  char* text, size_t text_size);
/* the QEMU case of word in state s from start */
int sail_add_case(SnxQrefBatch* b, u32 word, unsigned s, const CPU* start);
void sail_state_of(const CPU* c, SnxQrefState* s);

/* what a test holds its words to and how it runs them */
typedef struct {
    char** qemu;              /* QEMU's path, the runner's, a folder for its files; NULL with a reference */
    struct SailRef* ref;      /* the reference the words are held to, without QEMU */
    const char* record;       /* with QEMU: the file QEMU's ends of every state of the words are written to */
    const char* name;         /* the test's, in its lines */
    const char* const* forms; /* each form's host, in the lines: "this", "a plain" */
    unsigned nforms;
    int verbose; /* the differences' lines; 2: each run named before it starts */
    u8* work;    /* SAIL_WINDOW_SIZE bytes the native code's guest memory is (the test's, mapped where it must) */
    void* user;
    /* whether word i has code of form f (a word with none runs under QEMU alone, for --record) */
    int (*has)(void* user, size_t i, unsigned form);
    /* word i's native end of form f in state s from start, its guest memory work (sail_window_start first): a pair
       where start's pc is SAIL_PAIR_PC */
    void (*run)(void* user, size_t i, unsigned form, unsigned s, const CPU* start, u8* work, const u8* initial,
                SailEnd* e);
} SailHold;
/* the words against QEMU's ends, or the reference's hashes of them: how many runs differ (and a failure of QEMU's
   runner or of the record), *compared counting the runs; the words not held and those the reference holds none of
   are said, with an example */
size_t sail_hold(SailHold* h, const u32* words, size_t n, unsigned states, size_t* compared);

/* ---- what QEMU can't stand for: LLVM's ends, frozen (support/frozen.h) ----
   A word sail_unheld names, and a store-exclusive (STXR, STXP of doublewords) with its monitor armed at its address and
   a value the memory doesn't hold, which no QEMU case starts from: what LLVM's steps (llvm_steps.c) made of their
   states, while the LLVM back end was there, kept in tests/data/sail_frozen.txt (snx-test-sail-frozen --record). A
   file's item is a word, its hash its SAIL_STATES_MAX states' ends (sail_end_hash), folded. Its words are those of
   sail_candidates in the parts, in their order */
#define SAIL_FROZEN_GENERATOR 1u
enum { SAIL_FROZEN_UNHELD, SAIL_FROZEN_EXCLUSIVE, SAIL_FROZEN_PARTS };
/* word's part of a frozen file (-1: none) */
int sail_frozen_part(u32 word);
/* state s of word as its frozen part has it: sail_start's at SAIL_PC, a store-exclusive's monitor armed */
void sail_frozen_start(CPU* c, u32 word, unsigned s, const RtAPI* rt, const u8* initial);
/* the words the tests draw from (*out allocated): each row of the decoder's samples, and each again with Rd, Rn and
   Rm 31, that the guest decodes, in the same order for every test */
size_t sail_candidates(unsigned samples, u32** out);
/* the digest of the frozen parts' words, which a file's "inputs" must be */
u64 sail_frozen_inputs(const u32* candidates, size_t n);
/* word i's form f run in its SAIL_STATES_MAX frozen states, their ends' hashes folded */
u64 sail_frozen_word(SailHold* h, size_t i, unsigned form, u32 word, const u8* initial);
/* the words a test made (words[n], candidates' order) that a frozen part has, each form against path's hash: how
   many differ, the file's being wrong a failure too; *compared counts the runs */
size_t sail_frozen(SailHold* h, const char* path, const u32* words, size_t n, size_t* compared);

/* a reference: for each word, sorted, the hashes of its states' ends */
typedef struct SailRef {
    u32* words;
    u64* hashes; /* SAIL_STATES_MAX a word */
    size_t count;
    u64 generator; /* what it was recorded with (sail_generator) */
} SailRef;
/* what a reference's hashes depend on besides QEMU: the window, the pcs, the states' drawing, the hash */
u64 sail_generator(void);
int sail_ref_write(const char* path, const u32* words, const u64* hashes, size_t count, char* error, size_t size);
/* 0, or -1 with why in error (a file of another generator names what it was made with) */
int sail_ref_read(const char* path, SailRef* ref, char* error, size_t size);
/* word's hashes, NULL where the reference holds none */
const u64* sail_ref_find(const SailRef* ref, u32 word);
void sail_ref_free(SailRef* ref);
#endif
