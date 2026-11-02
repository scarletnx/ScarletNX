/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/video_core/macro.cpp, src/video_core/macro.h):
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project */
/* The macro engine (MME): the 3D engine's macros, interpreted a step at a time. A macro's state is its channel's
   (GpuMme), so that a method it sends that makes the stream wait (a report acquire) stops the macro right after it,
   and the macro goes on from there (mme_resume) when the wait ends, before the stream's next word. A delay slot is a
   step of its own, so such a stop can come between a branch or exit and its slot */
#include "nx/gpu/maxwell/engines_int.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/tracy.h"

static u32 fetch_param(GpuChannel* ch) {
    GpuMme* m = &ch->mme;
    return m->next < m->np ? ch->mme_params[m->next++] : 0;
}
static void set_reg(GpuMme* m, u32 r, u32 v) {
    if (r) m->regs[r] = v; /* r0 is zero */
}
/* the method address: the method in bits 0-11, the increment after each send in bits 12-17 */
static void set_method(GpuMme* m, u32 v) {
    m->method = v & 0xFFF;
    m->incr = (v >> 12) & 0x3F;
}
static void send(GpuChannel* ch, u32 v) {
    GpuMme* m = &ch->mme;
    u32 at = m->method;
    m->method = (m->method + m->incr) & 0xFFF;
    mme_out(ch, at, v);
}
/* the result operation (bits 4-6) of an ALU, add or bitfield step */
static void result(GpuChannel* ch, u32 op, u32 dst, u32 v) {
    GpuMme* m = &ch->mme;
    switch (op) {
        case 0: set_reg(m, dst, fetch_param(ch)); break; /* fetch, the result unused */
        case 1: set_reg(m, dst, v); break;
        case 2: /* move, and the method address */
            set_reg(m, dst, v);
            set_method(m, v);
            break;
        case 3: /* fetch, and send the result */
            set_reg(m, dst, fetch_param(ch));
            send(ch, v);
            break;
        case 4: /* move, and send it */
            set_reg(m, dst, v);
            send(ch, v);
            break;
        case 5: /* fetch, and the result as the method address */
            set_reg(m, dst, fetch_param(ch));
            set_method(m, v);
            break;
        case 6: /* move, the method address, and send a parameter */
            set_reg(m, dst, v);
            set_method(m, v);
            send(ch, fetch_param(ch));
            break;
        default: /* move, the method address, and send bits 12-17 of the result */
            set_reg(m, dst, v);
            set_method(m, v);
            send(ch, (v >> 12) & 0x3F);
            break;
    }
}
static u32 alu(GpuMme* m, u32 op, u32 a, u32 b) {
    u64 r;
    switch (op) {
        case 0: r = (u64)a + b; break;
        case 1: r = (u64)a + b + m->carry; break;
        case 2:
            r = (u64)a - b;
            m->carry = r < 0x100000000ull; /* no borrow */
            return (u32)r;
        case 3:
            r = (u64)a - b - (m->carry ? 0 : 1);
            m->carry = r < 0x100000000ull;
            return (u32)r;
        case 8: return a ^ b;
        case 9: return a | b;
        case 10: return a & b;
        case 11: return a & ~b;
        case 12: return ~(a & b);
        default: LOG_LIMIT(4, "gpu: macro ALU operation %u is unknown\n", op); return 0;
    }
    m->carry = r >> 32 != 0;
    return (u32)r;
}
/* one instruction */
static void step(GpuChannel* ch) {
    GpuMme* m = &ch->mme;
    u32 insn = ch->mme_code[m->pc & 0x1FFF], at = m->pc;
    u32 op = insn & 7, rop = (insn >> 4) & 7, dst = (insn >> 8) & 7;
    u32 a = m->regs[(insn >> 11) & 7], b = m->regs[(insn >> 14) & 7];
    u32 mask = (1u << ((insn >> 22) & 31)) - 1, src_bit = (insn >> 17) & 31, dst_bit = (insn >> 27) & 31;
    i32 imm = (i32)((insn >> 14) ^ 0x20000u) - 0x20000; /* bits 14-31, signed */
    int in_slot = m->slot, ends = m->exiting, taken = 0;
    m->pc++;
    m->slot = 0;
    m->exiting = 0;
    if (m->branch) { /* this is a taken branch's delay slot: after it, the target */
        m->branch = 0;
        m->pc = m->target;
    }
    switch (op) {
        case 0: result(ch, rop, dst, alu(m, (insn >> 17) & 31, a, b)); break;
        case 1: result(ch, rop, dst, a + (u32)imm); break;
        case 2: /* insert bits of b at src_bit into a at dst_bit */
            result(ch, rop, dst, (a & ~(mask << dst_bit)) | (((b >> src_bit) & mask) << dst_bit));
            break;
        case 3: /* extract at a register's bit, shift left by an immediate */
            result(ch, rop, dst, ((b >> (a & 31)) & mask) << dst_bit);
            break;
        case 4: /* extract at an immediate's bit, shift left by a register */
            result(ch, rop, dst, ((b >> src_bit) & mask) << (a & 31));
            break;
        case 5: set_reg(m, dst, mme_reg(ch, a + (u32)imm)); break; /* read a register */
        case 7: {                                                  /* branch on a zero (bit 4 clear) or not */
            int zero = !a;
            if (((insn >> 4) & 1) ? !zero : zero) {
                taken = 1;
                if ((insn >> 5) & 1) /* annulled: no delay slot */
                    m->pc = at + (u32)imm;
                else {
                    m->branch = 1;
                    m->target = at + (u32)imm;
                    m->slot = 1;
                }
            }
            break;
        }
        default: LOG_LIMIT(4, "gpu: macro operation %u is unknown\n", op); break;
    }
    if (ends) {
        /* this was an exit's delay slot. A taken branch here gets its own slot first */
        if (taken && m->branch)
            m->exiting = 1;
        else
            m->running = 0;
        return;
    }
    /* an exit (bit 7) ends the macro after its delay slot; not from inside a slot, and a taken branch's doesn't
       count. A branch not taken honours it, as Ryujinx's interpreter does */
    if (((insn >> 7) & 1) && !in_slot && !taken) {
        m->exiting = 1;
        m->slot = 1;
    }
}
/* runs the macro until it ends or a method it sent makes the stream wait */
static void mme_go(GpuChannel* ch) {
    GpuMme* m = &ch->mme;
    while (m->running && !ch->park) {
        if (++m->steps > MME_STEPS) {
            WARN_LIMIT(4, "gpu: macro on channel %u ran %u instructions and is cut off\n", ch->id, MME_STEPS);
            m->running = 0;
            return;
        }
        step(ch);
    }
}
void mme_resume(GpuChannel* ch) { mme_go(ch); }
/* runs macro idx through the interpreter */
static void mme_interpret(GpuChannel* ch, u32 idx, u32 np) {
    GpuMme* m = &ch->mme;
    memset(m, 0, sizeof *m);
    m->pc = ch->mme_start[idx & 0x7F];
    m->np = np;
    m->regs[1] = np ? ch->mme_params[0] : 0; /* the first parameter is in r1 */
    m->next = 1;
    m->running = 1;
    mme_go(ch);
}

/* ============================================================ uploads, sends and calls */
void mme_upload_ptr(GpuChannel* ch, u32 v) { ch->mme_upload = v; }
void mme_upload(GpuChannel* ch, u32 v) { ch->mme_code[ch->mme_upload++ & 0x1FFF] = v; }
void mme_bind(GpuChannel* ch, u32 v) { ch->mme_start[ch->mme_start_ptr++ & 0x7F] = v; }
void mme_out(GpuChannel* ch, u32 m, u32 v) {
    m &= 0xFFF;
    if (m >= 0xE00) /* a macro method only sets its register, as Ryujinx's interpreter does: starting a call over the
                       parameters the running macro is still reading is nothing the hardware does (Eden would nest
                       one) */
        ch->r3d[m] = v;
    else /* constant buffer data stays held for the words after it (r3d_const_upload), and the next method of any
            other kind writes it: a draw macro's base vertex and base instance went to guest memory as two writes,
            each with its renderer checks and stamps, before the draw and two more after it */
        maxwell3d_method(ch, m, v, !(m >= 0x8E4 && m <= 0x8F3));
}
u32 mme_reg(GpuChannel* ch, u32 a) { return ch->r3d[a & 0xFFF]; }
void mme_run(GpuChannel* ch, u32 idx, u32 np) {
    SNX_ZONE_V(z, "mme macro", idx);
    mme_interpret(ch, idx & 0x7F, np);
}
