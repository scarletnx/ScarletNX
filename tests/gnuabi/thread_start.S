/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright 2026 the ScarletNX contributors */
/* What the thread and signal fixtures (threads.c, signals.c, restart.c, sigsuspend.c) write in assembly */
.text

/* long spawn_thread(flags, stack_top, ptid, tls, ctid, fn, arg): clone(2), and the child calls fn(arg) on its stack
   and exits with its result */
.global spawn_thread
.type spawn_thread, %function
spawn_thread:
    stp x5, x6, [x1, #-16]!
    mov x8, #220
    svc #0
    cbnz x0, 1f
    ldp x5, x0, [sp], #16
    blr x5
    mov x8, #93
    svc #0
    brk #0
1:  ret
.size spawn_thread, .-spawn_thread

/* a restorer of the guest's own (SA_RESTORER): it marks restorer_used, then rt_sigreturn */
.global own_restorer
.type own_restorer, %function
own_restorer:
    adrp x9, restorer_used
    mov w10, #1
    str w10, [x9, :lo12:restorer_used]
    mov x8, #139
    svc #0
    brk #1
.size own_restorer, .-own_restorer

/* void clobber(void): the registers a call may change, all of them changed, and the flags */
.global clobber
.type clobber, %function
clobber:
    mov x1, #0xdead
    mov x2, x1
    mov x3, x1
    mov x4, x1
    mov x5, x1
    mov x6, x1
    mov x7, x1
    mov x9, x1
    mov x10, x1
    mov x11, x1
    mov x12, x1
    mov x13, x1
    mov x14, x1
    mov x15, x1
    mov x16, x1
    mov x17, x1
    movi v0.16b, #0xee
    movi v1.16b, #0xee
    movi v7.16b, #0xee
    movi v16.16b, #0xee
    movi v31.16b, #0xee
    mov x0, #1
    cmp x0, #2 /* N set, Z and C clear */
    ret
.size clobber, .-clobber

/* u64 regs_survive(tgid, tid, sig): tgkill with known values in x3-x17 (x8 is the call's number), v0, v1, v7, v16,
   v31 and the flags, which a handler between changes. The number of registers that came back different */
.global regs_survive
.type regs_survive, %function
regs_survive:
    stp x19, x20, [sp, #-16]!
    mov x3, #0x103
    mov x4, #0x104
    mov x5, #0x105
    mov x6, #0x106
    mov x7, #0x107
    mov x9, #0x109
    mov x10, #0x10a
    mov x11, #0x10b
    mov x12, #0x10c
    mov x13, #0x10d
    mov x14, #0x10e
    mov x15, #0x10f
    mov x16, #0x110
    mov x17, #0x111
    movi v0.16b, #0x10
    movi v1.16b, #0x11
    movi v7.16b, #0x17
    movi v16.16b, #0x26
    movi v31.16b, #0x41
    cmp x3, x3 /* Z and C set */
    mov x8, #131
    svc #0
    mov x19, #0
    cset x20, eq
    eor x20, x20, #1
    add x19, x19, x20
    cset x20, cs
    eor x20, x20, #1
    add x19, x19, x20
.macro same reg, value
    mov x20, #\value
    cmp \reg, x20
    cset x20, ne
    add x19, x19, x20
.endm
    same x3, 0x103
    same x4, 0x104
    same x5, 0x105
    same x6, 0x106
    same x7, 0x107
    same x9, 0x109
    same x10, 0x10a
    same x11, 0x10b
    same x12, 0x10c
    same x13, 0x10d
    same x14, 0x10e
    same x15, 0x10f
    same x16, 0x110
    same x17, 0x111
.macro vsame reg, byte
    umov x20, \reg\().d[1]
    mov x3, #(\byte * 0x0101)
    orr x3, x3, x3, lsl #16
    orr x3, x3, x3, lsl #32
    cmp x20, x3
    cset x20, ne
    add x19, x19, x20
.endm
    vsame v0, 0x10
    vsame v1, 0x11
    vsame v7, 0x17
    vsame v16, 0x26
    vsame v31, 0x41
    mov x0, x19
    ldp x19, x20, [sp], #16
    ret
.size regs_survive, .-regs_survive

/* u64 skip_after(tgid, tid, sig): tgkill, then x0 = 77 unless the handler moved the pc past that instruction */
.global skip_after
.type skip_after, %function
skip_after:
    mov x8, #131
    svc #0
    mov x0, #77
    ret
.size skip_after, .-skip_after
.bss
.balign 4
.global restorer_used
restorer_used:
    .word 0
.section .note.GNU-stack,"",%progbits
