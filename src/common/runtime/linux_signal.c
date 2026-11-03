/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The Linux AArch64 signal ABI (linux_signal.h) */
#include "common/runtime/linux_signal.h"
#include "common/utilities/bits.h"
#include "common/utilities/endian.h"
#include <string.h>

/* offsets in the frame: siginfo, then the ucontext */
enum {
    UC = SNX_LINUX_SIGFRAME_UCONTEXT,
    UC_FLAGS = UC + 0,
    UC_LINK = UC + 8,
    UC_STACK = UC + 16,   /* stack_t, 24 bytes */
    UC_SIGMASK = UC + 40, /* 8 bytes, then the rest of glibc's 128-byte sigset */
    MC = UC + 176,        /* struct sigcontext, 16-byte aligned */
    MC_FAULT = MC + 0,
    MC_REGS = MC + 8,     /* x0-x30 */
    MC_SP = MC + 256,
    MC_PC = MC + 264,
    MC_PSTATE = MC + 272,
    MC_RESERVED = MC + 288, /* 4096 bytes of records */
    RESERVED_SIZE = 4096,
};
typedef char check_size[MC_RESERVED + RESERVED_SIZE == SNX_LINUX_SIGFRAME_SIZE ? 1 : -1];

/* the records in sigcontext's reserved space: a magic and a size, then the record's own fields */
#define FPSIMD_MAGIC 0x46508001u
#define FPSIMD_SIZE  528u /* the header, fpsr, fpcr, 32 vector registers */
#define ESR_MAGIC    0x45535201u
#define ESR_SIZE     16u

void snx_linux_siginfo_encode(uint8_t out[SNX_LINUX_SIGINFO_SIZE], const SnxLinuxSiginfo* info) {
    memset(out, 0, SNX_LINUX_SIGINFO_SIZE);
    snx_st_le32(out + 0, (uint32_t)info->signo);
    snx_st_le32(out + 4, (uint32_t)info->error);
    snx_st_le32(out + 8, (uint32_t)info->code);
    /* the union at 16: the fault form's address, else pid (or timer id), uid (or overrun), then the value */
    bool fault = info->code > 0 && info->code != SNX_SI_KERNEL &&
                 (info->signo == 11 || info->signo == 7 || info->signo == 4 || info->signo == 8 || info->signo == 5);
    if (fault) {
        snx_st_le64(out + 16, info->addr);
        return;
    }
    snx_st_le32(out + 16, (uint32_t)info->pid);
    snx_st_le32(out + 20, info->uid);
    snx_st_le64(out + 24, info->value);
}

void snx_linux_altstack_encode(uint8_t out[24], const SnxLinuxAltStack* stack) {
    memset(out, 0, 24);
    snx_st_le64(out, stack->sp);
    snx_st_le32(out + 8, (uint32_t)stack->flags);
    snx_st_le64(out + 16, stack->size);
}
SnxLinuxAltStack snx_linux_altstack_decode(const uint8_t in[24]) {
    SnxLinuxAltStack s = {snx_ld_le64(in), (int32_t)snx_ld_le32(in + 8), snx_ld_le64(in + 16)};
    return s;
}
int32_t snx_linux_altstack_flags(const SnxLinuxAltStack* stack, uint64_t sp) {
    if (!stack->size || (stack->flags & SNX_SS_DISABLE)) return SNX_SS_DISABLE;
    return sp > stack->sp && sp - stack->sp <= stack->size ? SNX_SS_ONSTACK : 0;
}

void snx_linux_sigaction_encode(uint8_t out[32], const SnxLinuxSigaction* action) {
    snx_st_le64(out, action->handler);
    snx_st_le64(out + 8, action->flags);
    snx_st_le64(out + 16, action->restorer);
    snx_st_le64(out + 24, action->mask);
}
SnxLinuxSigaction snx_linux_sigaction_decode(const uint8_t in[32]) {
    SnxLinuxSigaction a = {snx_ld_le64(in), snx_ld_le64(in + 8), snx_ld_le64(in + 16), snx_ld_le64(in + 24)};
    return a;
}

SnxSigDefault snx_linux_signal_default(int sig) {
    switch (sig) {
    case 3: case 4: case 5: case 6: case 7: case 8: case 11: case 24: case 25: case 31:
        return SNX_SIGDFL_CORE; /* SIGQUIT, SIGILL, SIGTRAP, SIGABRT, SIGBUS, SIGFPE, SIGSEGV, SIGXCPU, SIGXFSZ, SIGSYS */
    case 17: case 23: case 28:
        return SNX_SIGDFL_IGNORE; /* SIGCHLD, SIGURG, SIGWINCH */
    case 19: case 20: case 21: case 22:
        return SNX_SIGDFL_STOP; /* SIGSTOP, SIGTSTP, SIGTTIN, SIGTTOU */
    case 18:
        return SNX_SIGDFL_CONT;
    default:
        return SNX_SIGDFL_TERM; /* the others, the real-time ones too */
    }
}

bool snx_linux_signal_ignored(int sig, const SnxLinuxSigaction* action) {
    if (sig == SNX_SIGKILL || sig == SNX_SIGSTOP) return false;
    if (action->handler == SNX_SIG_IGN) return true;
    return action->handler == SNX_SIG_DFL &&
           (snx_linux_signal_default(sig) == SNX_SIGDFL_IGNORE || snx_linux_signal_default(sig) == SNX_SIGDFL_CONT);
}

int snx_linux_signal_next(uint64_t pending, uint64_t mask) {
    uint64_t ready = pending & ~(mask & ~SNX_SIG_UNBLOCKABLE);
    if (!ready) return 0;
    static const int synchronous[] = {11, 7, 4, 5, 8, 31};
    for (size_t i = 0; i < sizeof synchronous / sizeof *synchronous; ++i)
        if (ready & SNX_SIGBIT(synchronous[i])) return synchronous[i];
    return (int)snx_ctz64(ready) + 1;
}

uint64_t snx_linux_sigframe_place(uint64_t sp, uint64_t flags, const SnxLinuxAltStack* stack, uint64_t* record) {
    if ((flags & SNX_SA_ONSTACK) && snx_linux_altstack_flags(stack, sp) == 0) sp = stack->sp + stack->size;
    *record = (sp - 16) & ~(uint64_t)15;
    return (*record - SNX_LINUX_SIGFRAME_SIZE) & ~(uint64_t)15;
}

void snx_linux_sigframe_build(uint8_t out[SNX_LINUX_SIGFRAME_SIZE], const CPU* c, const uint8_t siginfo[128],
                              uint64_t mask, const SnxLinuxAltStack* stack, uint64_t esr) {
    memset(out, 0, SNX_LINUX_SIGFRAME_SIZE);
    memcpy(out, siginfo, SNX_LINUX_SIGINFO_SIZE);
    SnxLinuxAltStack s = *stack;
    s.flags = snx_linux_altstack_flags(stack, c->x[31]);
    snx_linux_altstack_encode(out + UC_STACK, &s);
    snx_st_le64(out + UC_SIGMASK, mask);
    for (int i = 0; i < 31; ++i) snx_st_le64(out + MC_REGS + 8 * i, c->x[i]);
    snx_st_le64(out + MC_SP, c->x[31]);
    snx_st_le64(out + MC_PC, c->pc);
    uint64_t nzcv = (uint64_t)(c->n & 1) << 31 | (uint64_t)(c->z & 1) << 30 | (uint64_t)(c->cf & 1) << 29 |
                    (uint64_t)(c->vf & 1) << 28;
    snx_st_le64(out + MC_PSTATE, nzcv); /* EL0t, AArch64 */
    uint8_t* r = out + MC_RESERVED;
    snx_st_le32(r, FPSIMD_MAGIC);
    snx_st_le32(r + 4, FPSIMD_SIZE);
    snx_st_le32(r + 8, c->fpsr);
    snx_st_le32(r + 12, c->fpcr);
    for (int i = 0; i < 32; ++i) memcpy(r + 16 + 16 * i, c->v[i].b, 16); /* the guest's bytes, lane 0 first */
    r += FPSIMD_SIZE;
    if (esr) {
        snx_st_le32(r, ESR_MAGIC);
        snx_st_le32(r + 4, ESR_SIZE);
        snx_st_le64(r + 8, esr);
        snx_st_le64(out + MC_FAULT, snx_ld_le64(siginfo + 16));
        r += ESR_SIZE;
    }
    /* the terminator: a record of magic 0, size 0 (the bytes are zero already) */
}

void snx_linux_sigframe_record(uint8_t out[16], const CPU* c) {
    snx_st_le64(out, c->x[29]);
    snx_st_le64(out + 8, c->pc);
}

void snx_linux_sigframe_enter(CPU* c, int sig, uint64_t frame, uint64_t record, uint64_t handler, uint64_t restorer) {
    c->x[0] = (uint64_t)sig;
    c->x[1] = frame;
    c->x[2] = frame + UC;
    c->x[29] = record;
    c->x[30] = restorer;
    c->x[31] = frame;
    c->pc = handler;
    /* the kernel enters a handler with PSTATE's flags as they were, and BTYPE clear: NZCV stays */
}

bool snx_linux_sigframe_restore(CPU* c, const uint8_t frame[SNX_LINUX_SIGFRAME_SIZE], uint64_t* mask) {
    /* the records first: the FP/SIMD one must be there, and each one inside the space */
    const uint8_t* fpsimd = NULL;
    for (uint32_t at = 0; at + 8 <= RESERVED_SIZE;) {
        const uint8_t* r = frame + MC_RESERVED + at;
        uint32_t magic = snx_ld_le32(r), size = snx_ld_le32(r + 4);
        if (!magic) break;
        if (size < 8 || size % 16 || size > RESERVED_SIZE - at) return false;
        if (magic == FPSIMD_MAGIC) {
            if (size != FPSIMD_SIZE) return false;
            fpsimd = r;
        }
        at += size;
    }
    if (!fpsimd) return false;
    for (int i = 0; i < 31; ++i) c->x[i] = snx_ld_le64(frame + MC_REGS + 8 * i);
    c->x[31] = snx_ld_le64(frame + MC_SP);
    c->pc = snx_ld_le64(frame + MC_PC);
    uint64_t pstate = snx_ld_le64(frame + MC_PSTATE);
    c->n = (uint32_t)(pstate >> 31) & 1;
    c->z = (uint32_t)(pstate >> 30) & 1;
    c->cf = (uint32_t)(pstate >> 29) & 1;
    c->vf = (uint32_t)(pstate >> 28) & 1;
    c->fpsr = snx_ld_le32(fpsimd + 8);
    c->fpcr = snx_ld_le32(fpsimd + 12);
    for (int i = 0; i < 32; ++i) memcpy(c->v[i].b, fpsimd + 16 + 16 * i, 16);
    *mask = snx_ld_le64(frame + UC_SIGMASK) & ~SNX_SIG_UNBLOCKABLE;
    return true;
}

uint64_t snx_linux_handler_mask(uint64_t thread_mask, int sig, const SnxLinuxSigaction* action) {
    uint64_t m = thread_mask | action->mask;
    if (!(action->flags & SNX_SA_NODEFER)) m |= SNX_SIGBIT(sig);
    return m & ~SNX_SIG_UNBLOCKABLE;
}
