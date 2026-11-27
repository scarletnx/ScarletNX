/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The QEMU reference's AArch64 runner: a freestanding program for qemu-aarch64 that runs a batch of cases
   (qemu_ref_proto.h) and writes their end states. Usage: runner BATCH RESULTS.

   A case is entered and left through signal frames. The runner traps (brk in runner_start.S), and the handler,
   on a sigaltstack, saves the runner's context from the frame and writes the case's state into it: X0-X30, SP,
   NZCV, PC, and FPSR, FPCR and V0-V31 in the fpsimd record (and the SVE record's Z registers, which -cpu max
   restores over them). The return from the handler starts the word. Every slot of the code region but the word
   holds a brk pad, the window's pages are not executable, and the window is read-only while the case runs, so the
   word's next trap says what it did:
     SIGTRAP on a pad, or SIGSEGV fetching from anywhere but the word: where it went (fall-through or branch);
     SIGILL at the word: UNDEF; SIGTRAP at the word: BRK, or an SVC, which the runner replaced by a brk;
     SIGSEGV at the word on a window page not yet written: the first store there, which makes the page writable
     and runs the word again; any other fault at the word: a data abort.
   The handler then copies the frame into the result and puts the runner's context back. A case's steps run one
   word at a time this way; a free run (steps 0) places every word and runs until it leaves them. */
#include "qemu_ref_proto.h"

#include <stddef.h>

/* the Linux AArch64 interface, as the kernel's uapi headers have it */
enum {
    SYS_OPENAT = 56,
    SYS_CLOSE = 57,
    SYS_LSEEK = 62,
    SYS_READ = 63,
    SYS_WRITE = 64,
    SYS_EXIT_GROUP = 94,
    SYS_SETITIMER = 103,
    SYS_SIGALTSTACK = 132,
    SYS_RT_SIGACTION = 134,
    SYS_MUNMAP = 215,
    SYS_MMAP = 222,
    SYS_MPROTECT = 226
};
enum { SIG_ILL = 4, SIG_TRAP = 5, SIG_BUS = 7, SIG_FPE = 8, SIG_SEGV = 11, SIG_ALRM = 14 };
enum { PROT_R = 1, PROT_W = 2, PROT_X = 4 };
#define MAP_PRIVATE_ANON   0x22
#define MAP_FIXED_NOREPL   0x100000
#define SA_SIGINFO         0x4ul
#define SA_ONSTACK         0x08000000ul
#define SA_RESTORER        0x04000000ul
#define SA_RESTART         0x10000000ul
#define FPSIMD_MAGIC       0x46508001u
#define SVE_MAGIC          0x53564501u
#define SEGV_ACCERR        2

typedef struct {
    uint32_t magic, size;
} Record;
typedef struct {
    Record head;
    uint32_t fpsr, fpcr;
    uint64_t v[64]; /* each register's low half, then its high half */
} FpsimdRecord;
/* struct ucontext: flags, link, the stack, the 1024-bit signal mask, then the 16-byte aligned sigcontext */
typedef struct {
    uint64_t flags, link, ss_sp, ss_flags, ss_size;
    uint64_t sigmask[16];
    uint64_t align;
    uint64_t fault_address, regs[31], sp, pc, pstate;
    uint64_t align2;
    uint8_t reserved[4096];
} Ucontext;
typedef char snx_qref_uc_layout[(offsetof(Ucontext, fault_address) == 176 && offsetof(Ucontext, reserved) == 464)
                                    ? 1
                                    : -1];
typedef struct {
    int signo, error, code, pad;
    uint64_t addr;
} Siginfo;
typedef struct {
    void (*handler)(int, Siginfo*, void*);
    unsigned long flags;
    void (*restorer)(void);
    uint64_t mask;
} Sigaction;

/* a context the handler saves and loads: the frame's registers and its FP state */
typedef struct {
    uint64_t x[31], sp, pc, pstate;
    uint32_t fpsr, fpcr;
    uint64_t v[64];
} Context;

extern void snx_qref_enter(void);    /* runner_start.S: traps with the brk at its address, returns after the case */
extern void snx_qref_restorer(void); /* rt_sigreturn */

#define DIRTY_MAX ((SNX_QREF_MAX_WINDOW + SNX_QREF_MAX_CODE) / SNX_QREF_PAGE)
#define OUT_SIZE  0x200000u

/* the window and code region now mapped, and what made them */
typedef struct {
    uint64_t base, size, kind, seed, code, code_size, flags;
    uint64_t patch_bytes; /* the patches' encoded size in pristine_patch */
    int mapped;
} Layout;

static struct {
    Layout layout;
    Context host, test, end;
    uint64_t serial, alarm_serial;
    const uint32_t* words; /* the case's words, in the mapped batch */
    uint64_t slot;         /* step mode: the code slot holding the word */
    uint64_t self_loop;    /* the address a rewritten zero-offset branch lands on (slot + 8), or 0 */
    uint64_t program_end, svcs;
    int running, free_run, signo, sicode, outcome;
    uint64_t fault, landing, imm;
    uint64_t changed_bytes, truncated;
    uint64_t dirty[DIRTY_MAX];
    uint32_t dirty_count;
    uint64_t out_used;
    int out_fd;
    uint64_t pristine_patch[SNX_QREF_MAX_PATCH / 8 + 64];
    uint8_t pristine[SNX_QREF_MAX_WINDOW];
    uint8_t altstack[65536];
    uint8_t out[OUT_SIZE];
} g_runner;

static long sys6(long n, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    register uint64_t x0 __asm__("x0") = a;
    register uint64_t x1 __asm__("x1") = b;
    register uint64_t x2 __asm__("x2") = c;
    register uint64_t x3 __asm__("x3") = d;
    register uint64_t x4 __asm__("x4") = e;
    register uint64_t x5 __asm__("x5") = f;
    register long x8 __asm__("x8") = n;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5), "r"(x8) : "memory", "cc");
    return (long)x0;
}

/* what the compiler may call in a freestanding program */
void* memcpy(void* dst, const void* src, size_t n);
void* memset(void* dst, int c, size_t n);
void* memmove(void* dst, const void* src, size_t n);
int memcmp(const void* a, const void* b, size_t n);
void* memcpy(void* dst, const void* src, size_t n) {
    uint8_t* d = dst;
    const uint8_t* s = src;
    while (n--) *d++ = *s++;
    return dst;
}
void* memmove(void* dst, const void* src, size_t n) {
    uint8_t* d = dst;
    const uint8_t* s = src;
    if (d < s) return memcpy(dst, src, n);
    while (n--) d[n] = s[n];
    return dst;
}
void* memset(void* dst, int c, size_t n) {
    uint8_t* d = dst;
    while (n--) *d++ = (uint8_t)c;
    return dst;
}
int memcmp(const void* a, const void* b, size_t n) {
    const uint8_t *x = a, *y = b;
    for (; n; --n, ++x, ++y)
        if (*x != *y) return *x < *y ? -1 : 1;
    return 0;
}

static void say(const char* s) {
    size_t n = 0;
    while (s[n]) ++n;
    sys6(SYS_WRITE, 2, (uint64_t)s, n, 0, 0, 0);
}
static void die(const char* s) {
    say("qemu_ref runner: ");
    say(s);
    say("\n");
    sys6(SYS_EXIT_GROUP, 3, 0, 0, 0, 0, 0);
}

/* the records of the frame's reserved area: fpsimd always, SVE under a CPU with it */
static Record* record(Ucontext* uc, uint32_t magic) {
    uint8_t* p = uc->reserved;
    while (p + sizeof(Record) <= uc->reserved + sizeof(uc->reserved)) {
        Record* r = (Record*)p;
        if (!r->magic || r->size < sizeof(Record)) return NULL;
        if (r->magic == magic) return r;
        p += r->size;
    }
    return NULL;
}

static void save(const Ucontext* uc, const FpsimdRecord* fp, Context* c) {
    memcpy(c->x, uc->regs, sizeof(c->x));
    c->sp = uc->sp;
    c->pc = uc->pc;
    c->pstate = uc->pstate;
    c->fpsr = fp->fpsr;
    c->fpcr = fp->fpcr;
    memcpy(c->v, fp->v, sizeof(c->v));
}

/* the frame takes c; the SVE record's Z registers, when it holds them, take V with zeros above (as a NEON write
   leaves them), since sigreturn restores Z over V */
static void load(Ucontext* uc, FpsimdRecord* fp, const Context* c) {
    Record* sve = record(uc, SVE_MAGIC);
    memcpy(uc->regs, c->x, sizeof(c->x));
    uc->sp = c->sp;
    uc->pc = c->pc;
    uc->pstate = c->pstate;
    fp->fpsr = c->fpsr;
    fp->fpcr = c->fpcr;
    memcpy(fp->v, c->v, sizeof(c->v));
    if (sve && sve->size > 16) {
        uint32_t vl = *(uint16_t*)((uint8_t*)sve + 8), n;
        uint8_t* z = (uint8_t*)sve + 16;
        for (n = 0; n < 32 && 16 + (n + 1) * vl <= sve->size; ++n) {
            memset(z + n * vl, 0, vl);
            memcpy(z + n * vl, &c->v[2 * n], 16);
        }
    }
}

static int in_range(uint64_t a, uint64_t base, uint64_t size) { return a - base < size; }

/* the code slot at a is one of the case's words now */
static int is_word(uint64_t a) {
    if (g_runner.free_run) return in_range(a, g_runner.layout.code, g_runner.program_end - g_runner.layout.code);
    return a == g_runner.slot;
}

/* the case's own word at slot a, which is_word(a) */
static uint32_t word_at(uint64_t a) { return g_runner.words[(a - g_runner.layout.code) / 4]; }

/* the case is over: the frame is its end state, and becomes the runner's again */
static void stop(Ucontext* uc, FpsimdRecord* fp, const Siginfo* info, int outcome) {
    save(uc, fp, &g_runner.end);
    g_runner.signo = info->signo;
    g_runner.sicode = info->code;
    g_runner.outcome = outcome;
    g_runner.running = 0;
    load(uc, fp, &g_runner.host);
}

/* a page of the window or code region the word wrote: writable until the case ends */
static int make_dirty(uint64_t a) {
    const Layout* l = &g_runner.layout;
    uint64_t page = a & ~(SNX_QREF_PAGE - 1);
    int code = in_range(page, l->code, l->code_size);
    if (!code && (!in_range(page, l->base, l->size) || (l->flags & SNX_QREF_FULL_DIFF))) return 0;
    if (g_runner.dirty_count == DIRTY_MAX) return 0;
    if (sys6(SYS_MPROTECT, page, SNX_QREF_PAGE, PROT_R | PROT_W | (code ? PROT_X : 0), 0, 0, 0)) return 0;
    g_runner.dirty[g_runner.dirty_count++] = page;
    return 1;
}

static void handler(int sig, Siginfo* info, void* context) {
    Ucontext* uc = context;
    FpsimdRecord* fp = (FpsimdRecord*)record(uc, FPSIMD_MAGIC);
    uint64_t pc = uc->pc;
    if (!fp) die("no fpsimd record in the signal frame");
    if (sig == SIG_TRAP && pc == (uint64_t)snx_qref_enter && !g_runner.running) {
        save(uc, fp, &g_runner.host);
        g_runner.host.pc += 4;
        g_runner.test.pstate = (uc->pstate & ~0xF0000000ull) | (g_runner.test.pstate & 0xF0000000ull);
        load(uc, fp, &g_runner.test);
        g_runner.running = 1;
        return;
    }
    if (!g_runner.running) {
        if (sig == SIG_ALRM) return;
        die("a signal outside a case");
    }
    switch (sig) {
    case SIG_ALRM:
        if (g_runner.alarm_serial == g_runner.serial) {
            stop(uc, fp, info, SNX_QREF_TIMEOUT);
            return;
        }
        g_runner.alarm_serial = g_runner.serial;
        return;
    case SIG_TRAP:
        if (!is_word(pc)) {
            g_runner.landing = pc == g_runner.self_loop ? g_runner.slot : pc;
            stop(uc, fp, info, SNX_QREF_NEXT);
            return;
        }
        g_runner.imm = (word_at(pc) >> 5) & 0xFFFF;
        if ((word_at(pc) & 0xFFE0001Fu) == 0xD4000001u) { /* the pad standing in for an SVC */
            ++g_runner.svcs;
            uc->pc += 4;
            if (g_runner.free_run) return;
            g_runner.landing = pc + 4;
            stop(uc, fp, info, SNX_QREF_SVC);
            return;
        }
        if (g_runner.free_run && word_at(pc) == 0x14000000u) { /* "b ." */
            g_runner.imm = 0;
            g_runner.landing = pc;
            stop(uc, fp, info, SNX_QREF_BRANCH);
            return;
        }
        g_runner.landing = pc;
        stop(uc, fp, info, SNX_QREF_BRK);
        return;
    case SIG_SEGV:
        if (!is_word(pc)) { /* a fetch: the word went there */
            g_runner.landing = pc;
            stop(uc, fp, info, SNX_QREF_NEXT);
            return;
        }
        if (info->code == SEGV_ACCERR && make_dirty(info->addr)) return;
        g_runner.fault = info->addr;
        stop(uc, fp, info, SNX_QREF_FAULT);
        return;
    case SIG_ILL:
        stop(uc, fp, info, is_word(pc) ? SNX_QREF_UNDEF : SNX_QREF_FAULT);
        return;
    default:
        g_runner.fault = info->addr;
        stop(uc, fp, info, SNX_QREF_FAULT);
        return;
    }
}

/* the output, buffered */
static void out_flush(void) {
    uint64_t done = 0;
    while (done < g_runner.out_used) {
        long n = sys6(SYS_WRITE, (uint64_t)g_runner.out_fd, (uint64_t)g_runner.out + done, g_runner.out_used - done, 0,
                      0, 0);
        if (n <= 0) die("cannot write the results");
        done += (uint64_t)n;
    }
    g_runner.out_used = 0;
}
static void out64(uint64_t v) {
    if (g_runner.out_used + 8 > OUT_SIZE) out_flush();
    memcpy(g_runner.out + g_runner.out_used, &v, 8);
    g_runner.out_used += 8;
}
static void out_bytes(const uint8_t* p, uint64_t n) {
    uint64_t i;
    for (i = 0; i < n; i += 8) {
        uint64_t v = 0;
        memcpy(&v, p + i, n - i < 8 ? n - i : 8);
        out64(v);
    }
}

static void protect(uint64_t base, uint64_t size, int prot) {
    if (size && sys6(SYS_MPROTECT, base, size, (uint64_t)prot, 0, 0, 0)) die("mprotect failed");
}

static void unmap_layout(void) {
    Layout* l = &g_runner.layout;
    if (!l->mapped) return;
    if (l->size) sys6(SYS_MUNMAP, l->base, l->size, 0, 0, 0, 0);
    if (!in_range(l->code, l->base, l->size)) sys6(SYS_MUNMAP, l->code, l->code_size, 0, 0, 0, 0);
    l->mapped = 0;
}

static int map_at(uint64_t base, uint64_t size) {
    long p = sys6(SYS_MMAP, base, size, PROT_R | PROT_W, MAP_PRIVATE_ANON | MAP_FIXED_NOREPL, (uint64_t)-1, 0);
    return p == (long)base;
}

/* a code page's slots as they are when the case's words are not placed: pads */
static void pad(uint64_t page) {
    uint32_t* p = (uint32_t*)page;
    uint64_t i;
    for (i = 0; i < SNX_QREF_PAGE / 4; ++i) p[i] = SNX_QREF_PAD;
}

/* maps and fills the case's window and code region, unless they are the last case's; 0 when they can't be */
static int prepare_layout(const uint64_t* c, const uint64_t* patches, uint64_t patch_bytes) {
    Layout* l = &g_runner.layout;
    uint64_t base = c[SNX_QREF_C_WINDOW], size = c[SNX_QREF_C_WINDOW_SIZE];
    uint64_t code = c[SNX_QREF_C_CODE], code_size = c[SNX_QREF_C_CODE_SIZE], i;
    const uint64_t* p = patches;
    uint32_t n;
    if (l->mapped && l->base == base && l->size == size && l->kind == c[SNX_QREF_C_SEED_KIND] &&
        l->seed == c[SNX_QREF_C_SEED] && l->code == code && l->code_size == code_size &&
        l->flags == c[SNX_QREF_C_FLAGS] && l->patch_bytes == patch_bytes &&
        !memcmp(g_runner.pristine_patch, patches, patch_bytes))
        return 1;
    unmap_layout();
    if ((base | size | code | code_size) & (SNX_QREF_PAGE - 1) || size > SNX_QREF_MAX_WINDOW || !code_size ||
        code_size > SNX_QREF_MAX_CODE || c[SNX_QREF_C_SEED_KIND] >= SNX_QREF_SEED_COUNT ||
        patch_bytes > sizeof(g_runner.pristine_patch))
        return 0;
    /* the code region lies inside the window or apart from it */
    if (in_range(code, base, size) != in_range(code + code_size - 1, base, size)) return 0;
    if (size && !map_at(base, size)) return 0;
    if (!in_range(code, base, size) && !map_at(code, code_size)) {
        if (size) sys6(SYS_MUNMAP, base, size, 0, 0, 0, 0);
        return 0;
    }
    l->base = base;
    l->size = size;
    l->kind = c[SNX_QREF_C_SEED_KIND];
    l->seed = c[SNX_QREF_C_SEED];
    l->code = code;
    l->code_size = code_size;
    l->flags = c[SNX_QREF_C_FLAGS];
    l->patch_bytes = patch_bytes;
    l->mapped = 1;
    memcpy(g_runner.pristine_patch, patches, patch_bytes);
    for (i = 0; i < size; i += 8) {
        uint64_t v = snx_qref_seed64((unsigned)l->kind, l->seed, base, i);
        memcpy(g_runner.pristine + i, &v, 8);
    }
    for (n = 0; n < c[SNX_QREF_C_PATCHES]; ++n) {
        uint64_t at = p[0], len = p[1], k;
        const uint8_t* bytes = (const uint8_t*)(p + 2);
        for (k = 0; k < len; ++k)
            if (in_range(at + k, base, size)) g_runner.pristine[at + k - base] = bytes[k];
        p += 2 + (len + 7) / 8;
    }
    memcpy((void*)base, g_runner.pristine, size);
    for (i = 0; i < code_size; i += SNX_QREF_PAGE) pad(code + i);
    /* while a case runs: the window read-only (but for a full diff), the code read and execute */
    protect(base, size, (l->flags & SNX_QREF_FULL_DIFF) ? PROT_R | PROT_W : PROT_R);
    protect(code, code_size, PROT_R | PROT_X);
    return 1;
}

static void sync_code(uint64_t a) {
    __asm__ volatile("dc cvau, %0\n\tdsb ish\n\tic ivau, %0\n\tdsb ish\n\tisb" : : "r"(a) : "memory");
}

/* what the runner puts at a slot for word w: an SVC traps as a pad would, a free run's "b ." stops, and in a
   step a direct branch to itself goes to slot + 8 instead, a pad, so it stops (taken or not) */
static uint32_t placed(uint32_t w) {
    if ((w & 0xFFE0001Fu) == 0xD4000001u) return SNX_QREF_PAD;
    if (g_runner.free_run) return w == 0x14000000u ? SNX_QREF_SELF_LOOP : w;
    if ((w & 0x7C000000u) == 0x14000000u && !(w & 0x03FFFFFFu)) return w | 2;                 /* B, BL */
    if ((w & 0xFE000000u) == 0x54000000u && !(w & 0x00FFFFE0u)) return w | (2u << 5);         /* B.cond */
    if ((w & 0x7C000000u) == 0x34000000u && !(w & 0x00FFFFE0u)) return w | (2u << 5);         /* CBZ, CBNZ */
    if ((w & 0x7E000000u) == 0x36000000u && !(w & 0x0007FFE0u)) return w | (2u << 5);         /* TBZ, TBNZ */
    if ((w & 0x7E000000u) == 0x74000000u && !(w & 0x00003FE0u)) return w | (2u << 5); /* CB<cc>, CB<cc> of an integer */
    return w;
}

static void write_slot(uint64_t a, uint32_t w) {
    uint64_t page = a & ~(SNX_QREF_PAGE - 1);
    protect(page, SNX_QREF_PAGE, PROT_R | PROT_W);
    *(volatile uint32_t*)a = w;
    protect(page, SNX_QREF_PAGE, PROT_R | PROT_X);
    sync_code(a);
}

/* writes the changed runs of [base, base + size): now against was */
static void report_range(uint64_t base, const uint8_t* now, const uint8_t* was, uint64_t size) {
    uint64_t i = 0;
    while (i < size) {
        uint64_t start, end, gap;
        if (now[i] == was[i]) {
            ++i;
            continue;
        }
        start = i;
        end = i + 1;
        /* runs closer than 8 bytes join */
        for (gap = 0, i = end; i < size && gap < 8; ++i) {
            if (now[i] != was[i]) {
                end = i + 1;
                gap = 0;
            } else {
                ++gap;
            }
        }
        i = end;
        if (g_runner.changed_bytes + (end - start) > SNX_QREF_MAX_CHANGED) {
            g_runner.truncated = 1;
            continue;
        }
        out64(base + start);
        out64(end - start);
        out_bytes(now + start, end - start);
        g_runner.changed_bytes += end - start;
    }
}

/* reports and undoes what the case wrote: the window (whole for a full diff, else its dirty pages) against the
   pristine window, and dirty code pages against pads (the case's words are pads again by now) */
static void finish_memory(void) {
    const Layout* l = &g_runner.layout;
    uint32_t i;
    uint8_t pads[SNX_QREF_PAGE];
    if (l->flags & SNX_QREF_FULL_DIFF) {
        report_range(l->base, (const uint8_t*)l->base, g_runner.pristine, l->size);
        memcpy((void*)l->base, g_runner.pristine, l->size);
    }
    pad((uint64_t)pads);
    for (i = 0; i < g_runner.dirty_count; ++i) {
        uint64_t page = g_runner.dirty[i];
        int code = in_range(page, l->code, l->code_size);
        const uint8_t* was = code ? pads : g_runner.pristine + (page - l->base);
        report_range(page, (const uint8_t*)page, was, SNX_QREF_PAGE);
        memcpy((void*)page, was, SNX_QREF_PAGE);
        protect(page, SNX_QREF_PAGE, code ? PROT_R | PROT_X : PROT_R);
    }
    g_runner.dirty_count = 0;
}

static void state_in(const uint64_t* s, Context* c) {
    memcpy(c->x, s + SNX_QREF_S_X, sizeof(c->x));
    c->sp = s[SNX_QREF_S_SP];
    c->pstate = s[SNX_QREF_S_NZCV] & 0xF0000000ull;
    c->fpcr = (uint32_t)s[SNX_QREF_S_FPCR];
    c->fpsr = (uint32_t)s[SNX_QREF_S_FPSR];
    memcpy(c->v, s + SNX_QREF_S_V, sizeof(c->v));
}

/* one step or a free run from pc: the outcome, with g_runner.end and g_runner.landing set */
static int run_from(uint64_t pc) {
    uint32_t w, put;
    int outcome;
    if (!g_runner.free_run) {
        w = word_at(pc);
        put = placed(w);
        g_runner.slot = pc;
        g_runner.self_loop = put != w && put != SNX_QREF_PAD ? pc + 8 : 0;
        write_slot(pc, put);
    }
    g_runner.test.pc = pc;
    ++g_runner.serial;
    snx_qref_enter();
    outcome = g_runner.outcome;
    if (!g_runner.free_run) {
        write_slot(pc, SNX_QREF_PAD);
        if (outcome == SNX_QREF_NEXT && g_runner.landing != pc + 4) outcome = SNX_QREF_BRANCH;
    } else if (outcome == SNX_QREF_NEXT) {
        outcome = SNX_QREF_BRANCH; /* a free run ends by leaving its words */
    }
    if (outcome == SNX_QREF_UNDEF || outcome == SNX_QREF_FAULT || outcome == SNX_QREF_TIMEOUT)
        g_runner.landing = g_runner.end.pc;
    return outcome;
}

static void state_out(const Context* c) {
    unsigned i;
    for (i = 0; i < 31; ++i) out64(c->x[i]);
    out64(c->sp);
    out64(c->pstate & 0xF0000000ull);
    out64(c->fpcr);
    out64(c->fpsr);
    for (i = 0; i < 64; ++i) out64(c->v[i]);
}

/* runs one case and writes its result; returns the number of u64 the case took in the batch, 0 when malformed */
static uint64_t run_case(const uint64_t* c, uint64_t left) {
    const uint64_t* state = c + SNX_QREF_CASE_WORDS;
    uint64_t nwords = c[SNX_QREF_C_WORDS], used, patch_bytes = 0, pc = c[SNX_QREF_C_PC], steps = 0;
    const uint64_t* patches;
    uint64_t r[SNX_QREF_RESULT_WORDS] = {0};
    uint32_t n;
    int error = 0, outcome = SNX_QREF_ERROR;
    if (left < SNX_QREF_CASE_WORDS + SNX_QREF_STATE_WORDS) return 0;
    used = SNX_QREF_CASE_WORDS + SNX_QREF_STATE_WORDS + (nwords + 1) / 2;
    patches = c + used;
    for (n = 0; n < c[SNX_QREF_C_PATCHES]; ++n) {
        uint64_t k;
        if (used + 2 > left) return 0;
        k = 2 + (c[used + 1] + 7) / 8;
        patch_bytes += k * 8;
        used += k;
    }
    if (used > left) return 0;
    g_runner.words = (const uint32_t*)(state + SNX_QREF_STATE_WORDS);
    g_runner.free_run = c[SNX_QREF_C_STEPS] == 0;
    g_runner.svcs = g_runner.imm = g_runner.fault = 0;
    g_runner.signo = g_runner.sicode = 0;
    g_runner.changed_bytes = g_runner.truncated = 0;
    state_in(state, &g_runner.test);
    if (!nwords || nwords * 4 + 8 > c[SNX_QREF_C_CODE_SIZE])
        error = SNX_QREF_ERR_CASE;
    else if (!prepare_layout(c, patches, patch_bytes))
        error = SNX_QREF_ERR_MAP;
    g_runner.program_end = g_runner.layout.code + nwords * 4;
    if (!error && ((pc & 3) || !in_range(pc, g_runner.layout.code, nwords * 4))) error = SNX_QREF_ERR_PC;
    if (!error && g_runner.free_run) {
        for (n = 0; n < nwords; ++n) write_slot(g_runner.layout.code + 4ull * n, placed(g_runner.words[n]));
        outcome = run_from(pc);
        pc = g_runner.landing;
        g_runner.test = g_runner.end;
        for (n = 0; n < nwords; ++n) write_slot(g_runner.layout.code + 4ull * n, SNX_QREF_PAD);
    }
    /* steps, while each lands on another of the case's words */
    while (!error && !g_runner.free_run) {
        outcome = run_from(pc);
        pc = g_runner.landing;
        g_runner.test = g_runner.end;
        if (outcome != SNX_QREF_NEXT && outcome != SNX_QREF_BRANCH && outcome != SNX_QREF_SVC) break;
        if (++steps == c[SNX_QREF_C_STEPS] || (pc & 3) || !in_range(pc, g_runner.layout.code, nwords * 4)) break;
    }
    if (error) {
        g_runner.test.pc = pc;
        g_runner.imm = (uint64_t)error;
        outcome = SNX_QREF_ERROR;
    } else if (outcome != SNX_QREF_BRK && outcome != SNX_QREF_SVC) {
        g_runner.imm = 0;
    }
    r[SNX_QREF_R_OUTCOME] = (uint64_t)outcome;
    r[SNX_QREF_R_SIGNO] = (uint64_t)g_runner.signo;
    r[SNX_QREF_R_SICODE] = (uint64_t)(uint32_t)g_runner.sicode;
    r[SNX_QREF_R_PC] = pc;
    r[SNX_QREF_R_FAULT] = g_runner.fault;
    r[SNX_QREF_R_IMM] = g_runner.imm;
    r[SNX_QREF_R_STEPS] = steps;
    r[SNX_QREF_R_SVCS] = g_runner.svcs;
    r[SNX_QREF_R_PSTATE] = g_runner.test.pstate;
    for (n = 0; n < SNX_QREF_RESULT_WORDS; ++n) out64(r[n]);
    state_out(&g_runner.test);
    if (!error) finish_memory();
    out64(~0ull); /* the changes end */
    out64(g_runner.truncated);
    return used;
}

static void setup_signals(void) {
    static const int sigs[] = {SIG_ILL, SIG_TRAP, SIG_BUS, SIG_FPE, SIG_SEGV, SIG_ALRM};
    uint64_t stack[3];
    Sigaction sa;
    uint64_t timer[4] = {0, 50000, 0, 50000}; /* every 50 ms */
    unsigned i;
    stack[0] = (uint64_t)g_runner.altstack;
    stack[1] = 0;
    stack[2] = sizeof(g_runner.altstack);
    if (sys6(SYS_SIGALTSTACK, (uint64_t)stack, 0, 0, 0, 0, 0)) die("sigaltstack failed");
    sa.handler = handler;
    sa.flags = SA_SIGINFO | SA_ONSTACK | SA_RESTORER | SA_RESTART;
    sa.restorer = snx_qref_restorer;
    sa.mask = 0;
    for (i = 0; i < sizeof(sigs) / sizeof(sigs[0]); ++i) sa.mask |= 1ull << (sigs[i] - 1);
    for (i = 0; i < sizeof(sigs) / sizeof(sigs[0]); ++i)
        if (sys6(SYS_RT_SIGACTION, (uint64_t)sigs[i], (uint64_t)&sa, 0, 8, 0, 0)) die("sigaction failed");
    if (sys6(SYS_SETITIMER, 0, (uint64_t)timer, 0, 0, 0, 0)) die("setitimer failed");
}

void snx_qref_main(uint64_t* sp);
void snx_qref_main(uint64_t* sp) {
    char** argv = (char**)(sp + 1);
    long in, size, p;
    uint64_t cases, i, at = 2, left;
    const uint64_t* batch;
    if (sp[0] != 3) die("usage: runner BATCH RESULTS");
    in = sys6(SYS_OPENAT, (uint64_t)-100, (uint64_t)argv[1], 0, 0, 0, 0);
    if (in < 0) die("cannot open the batch");
    size = sys6(SYS_LSEEK, (uint64_t)in, 0, 2, 0, 0, 0);
    if (size < 16 || (size & 7)) die("the batch is not a batch");
    p = sys6(SYS_MMAP, 0, (uint64_t)size, PROT_R, 2, (uint64_t)in, 0);
    if (p < 0 && p > -4096) die("cannot map the batch");
    batch = (const uint64_t*)p;
    if (batch[0] != SNX_QREF_MAGIC) die("the batch is not a batch");
    g_runner.out_fd = (int)sys6(SYS_OPENAT, (uint64_t)-100, (uint64_t)argv[2], 01 | 0100 | 01000, 0644, 0, 0);
    if (g_runner.out_fd < 0) die("cannot create the results");
    setup_signals();
    cases = batch[1];
    left = (uint64_t)size / 8 - 2;
    out64(SNX_QREF_MAGIC);
    out64(cases);
    for (i = 0; i < cases; ++i) {
        uint64_t used = run_case(batch + at, left);
        if (!used) die("a malformed case");
        at += used;
        left -= used;
    }
    out_flush();
    sys6(SYS_CLOSE, (uint64_t)g_runner.out_fd, 0, 0, 0, 0, 0);
    sys6(SYS_EXIT_GROUP, 0, 0, 0, 0, 0, 0);
}
