/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* NCE's machine layer (nce_host.h): the outcall, the runs of native code, and the signals that stop and resume the
   guest's code. AArch64 Linux and Android only */
#if defined(__aarch64__) && defined(__linux__) && !defined(__ANDROID__)
/* Command line often defines (ppc64be linux) */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#endif
#include "nx/runtime/nce/nce_host.h"
#include "nx/runtime/nce/nce_patch.h"
#include <stdio.h>

#if !SNX_NCE_HOST
int nce_host_init(const NceHostConfig* config, u64* tls, int* indirect, u64* outcall, char* why, size_t size) {
    (void)config;
    *tls = *outcall = 0;
    *indirect = 0;
    snprintf(why, size, "the host is not AArch64 Linux");
    return -1;
}
#else
#include <errno.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>
#if defined(__ANDROID__)
#include <android/api-level.h>
#endif
#ifndef SYS_SECCOMP
#define SYS_SECCOMP 1 /* a seccomp trap's si_code (asm-generic/siginfo.h), which glibc's headers leave out */
#endif

/* where a run of native code came from on the host: the registers that the C code of nce_host_run keeps (x19 to x30,
   d8 to d15), its sp and its FPCR. The runtime's functions run on the host's stack below sp. The asm below reads
   these offsets */
typedef struct NceHost {
    u64 x[12]; /* x19 to x30 */
    u64 sp;
    u64 d[8];
    u64 fpcr;
    struct NceHost* prev; /* the run that this one nests in */
} NceHost;
typedef char nce_check_host[offsetof(NceHost, sp) == 96 && offsetof(NceHost, d) == 104 &&
                                    offsetof(NceHost, fpcr) == 168
                                ? 1
                                : -1];

/* the thread's CPU, which the stubs read through the host's TPIDR_EL0 (NceEnv.tls), and its innermost run. The asm
   reads cpu at 0 and host at 8. reads: the thread's count of counter reads, which the counter's stubs add to */
typedef struct {
    CPU* cpu;
    NceHost* host;
    u64* reads;
} NceTls;
typedef char nce_check_tls[offsetof(NceTls, reads) == NCE_TLS_READS ? 1 : -1];
/* Where the stubs and the outcall find the thread's NceTls: the word g_nce_tls bytes from TPIDR_EL0, the same in each
   thread. Direct (glibc): t_nce itself, in the executable's static TLS. Indirect: a pointer to t_nce. Android's
   runtime is a library that the app loads, whose TLS is emulated below API 29 and whose initial-exec TLS bionic
   refuses from API 29, so the word is bionic's TLS_SLOT_APP, the app's from Android 10 (API 29) and errno's before.
   SNX_NCE_INDIRECT builds the indirect form on glibc too, with an initial-exec pointer as the word, for its test */
#if defined(__ANDROID__) || (defined(SNX_NCE_INDIRECT) && SNX_NCE_INDIRECT)
#define NCE_INDIRECT 1
static _Thread_local NceTls t_nce;
#if defined(__ANDROID__)
#define NCE_SLOT_APP 16 /* TLS_SLOT_APP (2) from TPIDR_EL0, in bytes */
#else
static _Thread_local NceTls* t_word __attribute__((tls_model("initial-exec")));
#endif
#else
#define NCE_INDIRECT 0
static _Thread_local NceTls t_nce __attribute__((tls_model("initial-exec")));
#endif
/* the word's distance from TPIDR_EL0. Only the asm reads it, so it is external: clang takes a static one that C code
   never reads as unused (-Wunused-but-set-global) */
extern u64 g_nce_tls __attribute__((visibility("hidden")));
u64 g_nce_tls;
static NceHostConfig g_config;
static struct sigaction g_old_ill, g_old_trap, g_old_sys;
typedef char nce_check_bpf[sizeof(NceBpf) == sizeof(struct sock_filter) && offsetof(NceBpf, jt) == 2 &&
                                   offsetof(struct sock_filter, jt) == 2 && offsetof(NceBpf, k) == 4 &&
                                   offsetof(struct sock_filter, k) == 4
                               ? 1
                               : -1];
static int g_sig_enter, g_sig_kick;

#if NCE_INDIRECT
/* the word at off from this thread's TPIDR_EL0 */
static NceTls** word_at(u64 off) { return (NceTls**)(void*)((u8*)__builtin_thread_pointer() + off); }
#endif
/* this thread's NceTls: NULL (indirect) in a thread that never attached */
static NceTls* nce_tls(void) {
#if NCE_INDIRECT
    return *word_at(g_nce_tls);
#else
    return &t_nce;
#endif
}
static u64 word_offset(void) {
#if defined(__ANDROID__)
    return NCE_SLOT_APP;
#elif NCE_INDIRECT
    return (u64)((uintptr_t)&t_word - (uintptr_t)__builtin_thread_pointer());
#else
    return (u64)((uintptr_t)&t_nce - (uintptr_t)__builtin_thread_pointer());
#endif
}

/* the asm's entries, local to this file */
void nce_outcall(void) __attribute__((visibility("hidden")));
int nce_host_save(NceHost* h) __attribute__((visibility("hidden"), returns_twice));
_Noreturn void nce_host_jump(NceHost* h) __attribute__((visibility("hidden")));
CPU* nce_outcall_c(CPU* c, u64 code, u64 ret, u64 nzcv) __attribute__((visibility("hidden")));

/* the thread's NceTls into x17 (then its CPU pointer) and x9 from the word at x30 and x10: the word, or what it
   points to (indirect) */
#if NCE_INDIRECT
#define NCE_ASM_CPU   "    ldr x17, [x17]\n"
#define NCE_ASM_BLOCK "    ldr x9, [x9, x10]\n"
#else
#define NCE_ASM_CPU   ""
#define NCE_ASM_BLOCK "    add x9, x9, x10\n"
#endif

/* The outcall. The stub comes here with BLR X17 and the outcall's code in x16. It lowered the guest's sp by NCE_FRAME
   and keeps x16, x17 and x30 at sp + 0, 8 and 16; x30 is now the stub's next word, kept at sp + 24. The guest's state
   goes into its CPU (the offsets of cpu.h: x at 0, sp at 248, NZCV at 264, v at 280, FPCR at 792, FPSR at 796),
   nce_outcall_c runs on the host's stack with the host's FPCR, and the state of the CPU comes back. The stub loads
   x16, x17 and x30 from its frame, which nce_outcall_c wrote */
__asm__(".text\n"
        ".p2align 4\n"
        ".type nce_outcall, %function\n"
        "nce_outcall:\n"
        "    str x30, [sp, #24]\n"
        "    mrs x17, tpidr_el0\n"
        "    adrp x30, g_nce_tls\n"
        "    ldr x30, [x30, :lo12:g_nce_tls]\n"
        "    ldr x17, [x17, x30]\n" NCE_ASM_CPU "    stp x0, x1, [x17, #0]\n"
        "    stp x2, x3, [x17, #16]\n"
        "    stp x4, x5, [x17, #32]\n"
        "    stp x6, x7, [x17, #48]\n"
        "    stp x8, x9, [x17, #64]\n"
        "    stp x10, x11, [x17, #80]\n"
        "    stp x12, x13, [x17, #96]\n"
        "    stp x14, x15, [x17, #112]\n"
        "    stp x18, x19, [x17, #144]\n"
        "    stp x20, x21, [x17, #160]\n"
        "    stp x22, x23, [x17, #176]\n"
        "    stp x24, x25, [x17, #192]\n"
        "    stp x26, x27, [x17, #208]\n"
        "    stp x28, x29, [x17, #224]\n"
        "    ldp x0, x1, [sp]\n"
        "    stp x0, x1, [x17, #128]\n"
        "    ldr x0, [sp, #16]\n"
        "    add x1, sp, #32\n"
        "    stp x0, x1, [x17, #240]\n"
        "    add x0, x17, #280\n"
        "    stp q0, q1, [x0, #0]\n"
        "    stp q2, q3, [x0, #32]\n"
        "    stp q4, q5, [x0, #64]\n"
        "    stp q6, q7, [x0, #96]\n"
        "    stp q8, q9, [x0, #128]\n"
        "    stp q10, q11, [x0, #160]\n"
        "    stp q12, q13, [x0, #192]\n"
        "    stp q14, q15, [x0, #224]\n"
        "    stp q16, q17, [x0, #256]\n"
        "    stp q18, q19, [x0, #288]\n"
        "    stp q20, q21, [x0, #320]\n"
        "    stp q22, q23, [x0, #352]\n"
        "    stp q24, q25, [x0, #384]\n"
        "    stp q26, q27, [x0, #416]\n"
        "    stp q28, q29, [x0, #448]\n"
        "    stp q30, q31, [x0, #480]\n"
        "    mrs x0, fpcr\n"
        "    str w0, [x17, #792]\n"
        "    mrs x0, fpsr\n"
        "    str w0, [x17, #796]\n"
        "    mrs x3, nzcv\n"
        "    ldr x2, [sp, #24]\n"
        "    mrs x9, tpidr_el0\n"
        "    adrp x10, g_nce_tls\n"
        "    ldr x10, [x10, :lo12:g_nce_tls]\n" NCE_ASM_BLOCK "    ldr x9, [x9, #8]\n"
        "    ldr x10, [x9, #96]\n"
        "    mov sp, x10\n"
        "    ldr x10, [x9, #168]\n"
        "    msr fpcr, x10\n"
        "    mov x0, x17\n"
        "    mov x1, x16\n"
        "    bl nce_outcall_c\n"
        "    mov x17, x0\n"
        "    ldr x0, [x17, #248]\n"
        "    sub sp, x0, #32\n"
        "    ldr w0, [x17, #264]\n"
        "    ldr w1, [x17, #268]\n"
        "    ldr w2, [x17, #272]\n"
        "    ldr w3, [x17, #276]\n"
        "    lsl w0, w0, #31\n"
        "    orr w0, w0, w1, lsl #30\n"
        "    orr w0, w0, w2, lsl #29\n"
        "    orr w0, w0, w3, lsl #28\n"
        "    msr nzcv, x0\n"
        "    ldr w0, [x17, #792]\n"
        "    msr fpcr, x0\n"
        "    ldr w0, [x17, #796]\n"
        "    msr fpsr, x0\n"
        "    add x0, x17, #280\n"
        "    ldp q0, q1, [x0, #0]\n"
        "    ldp q2, q3, [x0, #32]\n"
        "    ldp q4, q5, [x0, #64]\n"
        "    ldp q6, q7, [x0, #96]\n"
        "    ldp q8, q9, [x0, #128]\n"
        "    ldp q10, q11, [x0, #160]\n"
        "    ldp q12, q13, [x0, #192]\n"
        "    ldp q14, q15, [x0, #224]\n"
        "    ldp q16, q17, [x0, #256]\n"
        "    ldp q18, q19, [x0, #288]\n"
        "    ldp q20, q21, [x0, #320]\n"
        "    ldp q22, q23, [x0, #352]\n"
        "    ldp q24, q25, [x0, #384]\n"
        "    ldp q26, q27, [x0, #416]\n"
        "    ldp q28, q29, [x0, #448]\n"
        "    ldp q30, q31, [x0, #480]\n"
        "    ldp x18, x19, [x17, #144]\n"
        "    ldp x20, x21, [x17, #160]\n"
        "    ldp x22, x23, [x17, #176]\n"
        "    ldp x24, x25, [x17, #192]\n"
        "    ldp x26, x27, [x17, #208]\n"
        "    ldp x28, x29, [x17, #224]\n"
        "    ldp x2, x3, [x17, #16]\n"
        "    ldp x4, x5, [x17, #32]\n"
        "    ldp x6, x7, [x17, #48]\n"
        "    ldp x8, x9, [x17, #64]\n"
        "    ldp x10, x11, [x17, #80]\n"
        "    ldp x12, x13, [x17, #96]\n"
        "    ldp x14, x15, [x17, #112]\n"
        "    ldp x0, x1, [x17, #0]\n"
        "    ldr x30, [sp, #24]\n"
        "    ret\n"
        ".size nce_outcall, . - nce_outcall\n"
        /* setjmp's work for nce_host_run, without the C library's mangled sp, which the outcall reads */
        ".p2align 4\n"
        ".type nce_host_save, %function\n"
        "nce_host_save:\n"
        "    stp x19, x20, [x0, #0]\n"
        "    stp x21, x22, [x0, #16]\n"
        "    stp x23, x24, [x0, #32]\n"
        "    stp x25, x26, [x0, #48]\n"
        "    stp x27, x28, [x0, #64]\n"
        "    stp x29, x30, [x0, #80]\n"
        "    mov x1, sp\n"
        "    str x1, [x0, #96]\n"
        "    stp d8, d9, [x0, #104]\n"
        "    stp d10, d11, [x0, #120]\n"
        "    stp d12, d13, [x0, #136]\n"
        "    stp d14, d15, [x0, #152]\n"
        "    mrs x1, fpcr\n"
        "    str x1, [x0, #168]\n"
        "    mov x0, #0\n"
        "    ret\n"
        ".size nce_host_save, . - nce_host_save\n"
        /* longjmp's: back from h's nce_host_save, which returns 1 */
        ".p2align 4\n"
        ".type nce_host_jump, %function\n"
        "nce_host_jump:\n"
        "    ldp x19, x20, [x0, #0]\n"
        "    ldp x21, x22, [x0, #16]\n"
        "    ldp x23, x24, [x0, #32]\n"
        "    ldp x25, x26, [x0, #48]\n"
        "    ldp x27, x28, [x0, #64]\n"
        "    ldp x29, x30, [x0, #80]\n"
        "    ldr x1, [x0, #96]\n"
        "    mov sp, x1\n"
        "    ldp d8, d9, [x0, #104]\n"
        "    ldp d10, d11, [x0, #120]\n"
        "    ldp d12, d13, [x0, #136]\n"
        "    ldp d14, d15, [x0, #152]\n"
        "    ldr x1, [x0, #168]\n"
        "    msr fpcr, x1\n"
        "    mov x0, #1\n"
        "    ret\n"
        ".size nce_host_jump, . - nce_host_jump\n");

static u32 nzcv_of(const CPU* c) {
    return (u32)(c->n & 1u) << 31 | (u32)(c->z & 1u) << 30 | (u32)(c->cf & 1u) << 29 | (u32)(c->vf & 1u) << 28;
}
static void nzcv_set(CPU* c, u64 nzcv) {
    c->n = (u32)(nzcv >> 31) & 1u;
    c->z = (u32)(nzcv >> 30) & 1u;
    c->cf = (u32)(nzcv >> 29) & 1u;
    c->vf = (u32)(nzcv >> 28) & 1u;
}

/* An outcall, on the host's stack. The stub's frame is below the guest's sp: it gets the registers that the stub
   loads from it, and the stub's return, which the asm loads. A run that dispatch nests starts at the guest's sp, so
   its own stubs write over the frame before that. A stub that goes back with a B goes back to its site's next word, when the runtime left the guest
   there; a hook's or a trap's goes on at c->pc through x16, the register that a call may change */
CPU* nce_outcall_c(CPU* c, u64 code, u64 ret, u64 nzcv) {
    u64 sp = c->x[31], back;
    u8* frame;
    u32 word;
    memcpy(&word, (const void*)(uintptr_t)(ret + 12), sizeof word);
    back = nce_stub_back(word, ret);
    nzcv_set(c, nzcv);
    c->pc = back;
    g_config.dispatch(c, code);
    if (c->x[31] != sp || (back && c->pc != back)) nce_host_resume(c); /* the guest goes on elsewhere */
    frame = (u8*)(uintptr_t)(sp - NCE_FRAME);
    memcpy(frame, back ? &c->x[16] : &c->pc, 8);
    memcpy(frame + 8, &c->x[17], 8);
    memcpy(frame + 16, &c->x[30], 8);
    memcpy(frame + 24, &ret, 8); /* a nested run's stubs used the frame too: its sp starts at the guest's */
    return c;
}

/* ---- the signal's context ---- */

static struct fpsimd_context* fpsimd_of(ucontext_t* uc) {
    struct _aarch64_ctx* h = (struct _aarch64_ctx*)(void*)uc->uc_mcontext.__reserved;
    while (h->magic && h->size) {
        if (h->magic == FPSIMD_MAGIC) return (struct fpsimd_context*)(void*)h;
        h = (struct _aarch64_ctx*)(void*)((u8*)h + h->size);
    }
    return NULL;
}

/* the context's guest state into c */
static void context_load(ucontext_t* uc, CPU* c) {
    mcontext_t* m = &uc->uc_mcontext;
    struct fpsimd_context* f = fpsimd_of(uc);
    for (int i = 0; i < 31; i++) c->x[i] = m->regs[i];
    c->x[31] = m->sp;
    c->pc = m->pc;
    nzcv_set(c, m->pstate);
    if (f) {
        memcpy(c->v, f->vregs, sizeof c->v);
        c->fpsr = f->fpsr;
        c->fpcr = f->fpcr;
    }
}

/* c's state into the context, which sigreturn loads */
static void context_store(ucontext_t* uc, const CPU* c) {
    mcontext_t* m = &uc->uc_mcontext;
    struct fpsimd_context* f = fpsimd_of(uc);
    for (int i = 0; i < 31; i++) m->regs[i] = c->x[i];
    m->sp = c->x[31];
    m->pc = c->pc;
    /* NZCV, and no BTYPE: the guest's code goes on at an instruction, not at the target of a branch */
    m->pstate = (m->pstate & ~(0xF0000000ull | 0xC00ull)) | nzcv_of(c);
    if (f) {
        memcpy(f->vregs, c->v, sizeof c->v);
        f->fpsr = c->fpsr;
        f->fpcr = c->fpcr;
    }
}

static int in_guest(u64 pc) { return pc - g_config.lo < g_config.hi - g_config.lo; }

static void preempted(CPU* c, int sig) {
    (void)sig;
    g_config.preempted(c);
    nce_host_resume(c);
}
static void trapped(CPU* c, int sig) {
    g_config.trapped(c, sig);
    nce_host_resume(c);
}

/* the stopped guest's state into its CPU, and sigreturn into fn(c, sig) on the host's stack of the innermost run,
   with the host's FPCR. fn never returns: x30 and the frame pointer are 0 */
static void redirect(ucontext_t* uc, void (*fn)(CPU*, int), int sig) {
    CPU* c = nce_tls()->cpu;
    const NceHost* h = nce_tls()->host;
    mcontext_t* m = &uc->uc_mcontext;
    struct fpsimd_context* f;
    context_load(uc, c);
    m->regs[0] = (u64)(uintptr_t)c;
    m->regs[1] = (u64)sig;
    m->regs[29] = m->regs[30] = 0;
    m->sp = h->sp;
    m->pc = (u64)(uintptr_t)fn;
    m->pstate &= ~0xC00ull;
    f = fpsimd_of(uc);
    if (f) f->fpcr = (u32)h->fpcr;
}

static int stopped_guest(const ucontext_t* uc) {
    const NceTls* n = nce_tls();
    return n && n->cpu && n->host && in_guest(uc->uc_mcontext.pc);
}

static void enter_signal(int sig, siginfo_t* si, void* ctx) {
    (void)sig;
    (void)si;
    context_store((ucontext_t*)ctx, nce_tls()->cpu);
}
static void kick_signal(int sig, siginfo_t* si, void* ctx) {
    (void)si;
    if (stopped_guest((ucontext_t*)ctx)) redirect((ucontext_t*)ctx, preempted, sig);
}
/* a signal that is not the guest's, to the handler before ours */
static void chain(int sig, siginfo_t* si, void* ctx, const struct sigaction* old) {
    if (old->sa_flags & SA_SIGINFO) {
        if (old->sa_sigaction) old->sa_sigaction(sig, si, ctx);
    } else if (old->sa_handler == SIG_DFL) {
        signal(sig, SIG_DFL); /* the instruction raises it again, and the process ends as it would have */
    } else if (old->sa_handler != SIG_IGN) {
        old->sa_handler(sig);
    }
}
/* SIGILL and SIGTRAP: the guest's (an instruction it has no use for, a BRK) go to the runtime, the host's to the
   handler before this one */
static void trap_signal(int sig, siginfo_t* si, void* ctx) {
    if (stopped_guest((ucontext_t*)ctx)) {
        redirect((ucontext_t*)ctx, trapped, sig);
        return;
    }
    chain(sig, si, ctx, sig == SIGILL ? &g_old_ill : &g_old_trap);
}
/* SIGSYS from the net (nce_host_net): an SVC of the guest's that no stub holds, its system call not made. The
   context's pc is past it, and the runtime gets the SVC's own */
static void sys_signal(int sig, siginfo_t* si, void* ctx) {
    ucontext_t* uc = (ucontext_t*)ctx;
    if (si->si_code == SYS_SECCOMP && stopped_guest(uc)) {
        uc->uc_mcontext.pc -= 4;
        redirect(uc, trapped, sig);
        return;
    }
    chain(sig, si, ctx, &g_old_sys);
}

static int handle(int sig, void (*fn)(int, siginfo_t*, void*), struct sigaction* old) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = fn;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaddset(&sa.sa_mask, g_sig_enter);
    sigaddset(&sa.sa_mask, g_sig_kick);
    return sigaction(sig, &sa, old);
}

int nce_host_check(char* why, size_t size) {
#if NCE_INDIRECT
    const NceTls* held = *word_at(word_offset());
#if defined(__ANDROID__)
    int api = android_get_device_api_level();
    if (api < NCE_ANDROID_API) {
        snprintf(why, size,
                 "Android's API level is %d, and the thread's word that NCE's stubs read (bionic's TLS_SLOT_APP) is "
                 "the app's from API %d (Android 10), errno's before",
                 api, NCE_ANDROID_API);
        return -1;
    }
#endif
    if (held && held != &t_nce) {
        snprintf(why, size,
                 "the thread's word that NCE's stubs read (TPIDR_EL0 + %llu) holds %#llx, which another library of the "
                 "process put there",
                 (unsigned long long)word_offset(), (unsigned long long)(uintptr_t)held);
        return -1;
    }
#endif
    (void)why;
    (void)size;
    return 0;
}

int nce_host_init(const NceHostConfig* config, u64* tls, int* indirect, u64* outcall, char* why, size_t size) {
    u64 off = word_offset();
    void (*entry)(void) = nce_outcall;
    *tls = *outcall = 0;
    *indirect = 0;
    if (nce_host_check(why, size)) return -1;
    /* the stubs reach the word with ADD #imm12, LSL #12 and LDR #imm12, or with the LDR alone (indirect), scaled by 8 */
    if (off >= (NCE_INDIRECT ? 1u << 15 : 1u << 24) || (off & 7)) {
        snprintf(why, size, "the thread's CPU pointer is %#llx bytes from the thread pointer, out of the stubs' reach",
                 (unsigned long long)off);
        return -1;
    }
    g_nce_tls = off;
    g_config = *config;
    g_sig_enter = SIGRTMIN + 4;
    g_sig_kick = SIGRTMIN + 5;
    if (g_sig_kick > SIGRTMAX || handle(g_sig_enter, enter_signal, NULL) || handle(g_sig_kick, kick_signal, NULL) ||
        handle(SIGILL, trap_signal, &g_old_ill) || handle(SIGTRAP, trap_signal, &g_old_trap) ||
        handle(SIGSYS, sys_signal, &g_old_sys)) {
        snprintf(why, size, "the signal handlers could not be installed");
        return -1;
    }
    *tls = off;
    *indirect = NCE_INDIRECT;
    memcpy(outcall, &entry, sizeof *outcall); /* ISO C has no conversion from a function pointer to an integer */
    return 0;
}

int nce_host_attach(CPU* c, u64* reads, char* why, size_t size) {
#if NCE_INDIRECT
    NceTls** word = word_at(g_nce_tls);
    if (nce_host_check(why, size)) return -1; /* another library took the word in this thread */
    *word = &t_nce;
#endif
    (void)why;
    (void)size;
    t_nce.cpu = c;
    t_nce.reads = reads;
    return 0;
}

int nce_host_net(char* why, size_t size) {
    NceBpf prog[NCE_NET_WORDS];
    struct sock_fprog fp;
    long r;
    fp.len = (unsigned short)nce_net_filter(g_config.lo, g_config.hi, AUDIT_ARCH_AARCH64, prog);
    fp.filter = (struct sock_filter*)(void*)prog;
    /* a filter of an unprivileged process needs no_new_privs: a setuid program it runs gains nothing */
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) {
        snprintf(why, size, "prctl(PR_SET_NO_NEW_PRIVS) failed: %s", strerror(errno));
        return -1;
    }
    /* every thread of the process, those made before too: a guest thread may come from any */
    r = syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_TSYNC, &fp);
    if (r < 0) {
        snprintf(why, size, "seccomp(SECCOMP_SET_MODE_FILTER) failed: %s", strerror(errno));
        return -1;
    }
    if (r > 0) {
        snprintf(why, size, "thread %ld of the process cannot take the seccomp filter (another filter of its own)", r);
        return -1;
    }
    return 0;
}

int nce_host_tid(void) { return (int)syscall(SYS_gettid); }

void nce_host_kick(int tid) {
    if (tid > 0) syscall(SYS_tgkill, (long)getpid(), (long)tid, (long)g_sig_kick);
}

void nce_host_resume(CPU* c) {
    nce_tls()->cpu = c;
    syscall(SYS_tgkill, (long)getpid(), (long)nce_host_tid(), (long)g_sig_enter);
    abort(); /* enter_signal's sigreturn went to the guest */
}

void nce_host_run(CPU* c) {
    NceHost h;
    h.prev = nce_tls()->host;
    if (!nce_host_save(&h)) {
        nce_tls()->host = &h;
        nce_host_resume(c);
    }
    nce_tls()->host = h.prev;
}

void nce_host_leave(void) { nce_host_jump(nce_tls()->host); }

int nce_host_context(const void* context, CPU* c) {
    ucontext_t* uc = (ucontext_t*)(uintptr_t)context;
    const NceTls* n = nce_tls();
    if (!n || !n->cpu || !in_guest(uc->uc_mcontext.pc)) return 0;
    context_load(uc, c);
    return 1;
}
#endif
