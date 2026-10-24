/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported in part from QEMU (target/arm/tcg/translate.c, vec_helper.c, vfp_helper.c, crypto_helper.c),
   LGPL-2.1-or-later:
   Copyright (c) 2003 Fabrice Bellard
   Copyright (c) 2005-2007 CodeSourcery
   Copyright (c) 2007 OpenedHand, Ltd.
   Copyright (c) 2018 Linaro
   Copyright (C) 2013 - 2018 Linaro Ltd <ard.biesheuvel@linaro.org> */
#include "common/cpu/arm/llvm.h"
#include "common/cpu/arm/simd.h"
#include <limits.h>
#include <llvm-c/Target.h>

/* All guest arithmetic is i32 without nsw/nuw. In particular address arithmetic
   wraps BEFORE extending to a host pointer. Never use inbounds GEP for guests. */
typedef struct {
    LLVMContextRef ctx;
    LLVMModuleRef mod;
    LLVMBuilderRef b;
    LLVMValueRef fn, cpu, mem;
    LLVMTypeRef i1, i8, i16, i32, i64, ptr;
    uint32_t pc, word;
    uint32_t next; /* the next instruction's address */
    int thumb;     /* a Thumb block: an A32 emitter running a Thumb32 instruction's A32 twin reads Thumb's PC */
    int in_it;     /* the Thumb instruction being emitted is in an IT block: its 16-bit data processing sets no flags */
    unsigned pair; /* a Thumb32 doubleword exclusive's second register + 1 (TH_A32's .ra); 0: A32's first + 1 */
    SnxArmProfile profile;
    SnxArmMemoryMode memory;
    LLVMValueRef transfer_base, transfer_buffer;
    unsigned transfer_bytes, transfer_operation;
} ArmIR;

static int options_valid(const SnxArmCodegenOptions* o) {
    return !o || (o->abi == SNX_ARM_CODEGEN_OPTIONS_ABI &&
                  (o->profile == SNX_ARM_PROFILE_A57 || o->profile == SNX_ARM_PROFILE_ARM11 ||
                   o->profile == SNX_ARM_PROFILE_A9 ||
                   (o->profile == SNX_ARM_PROFILE_ARM7TDMI && o->memory == SNX_ARM_MEMORY_FLAT)) &&
                  (o->memory == SNX_ARM_MEMORY_FLAT || o->memory == SNX_ARM_MEMORY_CHECKED));
}
/* the AGB board's rules (arm.h's SNX_ARM_PROFILE_ARM7TDMI), which no other profile follows */
static int arm7tdmi(const ArmIR* a) { return a->profile == SNX_ARM_PROFILE_ARM7TDMI; }
static LLVMValueRef host_order(ArmIR* a, LLVMValueRef v, unsigned bytes);

static LLVMValueRef c32(ArmIR* a, uint32_t n) { return LLVMConstInt(a->i32, n, 0); }
static LLVMValueRef c64(ArmIR* a, uint64_t n) { return LLVMConstInt(a->i64, n, 0); }
static LLVMValueRef add(ArmIR* a, LLVMValueRef x, LLVMValueRef y) { return LLVMBuildAdd(a->b, x, y, ""); }
static LLVMValueRef sub(ArmIR* a, LLVMValueRef x, LLVMValueRef y) { return LLVMBuildSub(a->b, x, y, ""); }
static LLVMValueRef band(ArmIR* a, LLVMValueRef x, LLVMValueRef y) { return LLVMBuildAnd(a->b, x, y, ""); }
static LLVMValueRef bor(ArmIR* a, LLVMValueRef x, LLVMValueRef y) { return LLVMBuildOr(a->b, x, y, ""); }
static LLVMValueRef bxor(ArmIR* a, LLVMValueRef x, LLVMValueRef y) { return LLVMBuildXor(a->b, x, y, ""); }
static LLVMValueRef shl(ArmIR* a, LLVMValueRef x, unsigned n) { return LLVMBuildShl(a->b, x, c32(a, n), ""); }
static LLVMValueRef lsr(ArmIR* a, LLVMValueRef x, unsigned n) { return LLVMBuildLShr(a->b, x, c32(a, n), ""); }
static LLVMValueRef cmp(ArmIR* a, LLVMIntPredicate p, LLVMValueRef x, LLVMValueRef y) {
    return LLVMBuildICmp(a->b, p, x, y, "");
}
static LLVMValueRef choose(ArmIR* a, LLVMValueRef c, LLVMValueRef t, LLVMValueRef f) {
    return LLVMBuildSelect(a->b, c, t, f, "");
}
static LLVMValueRef z32(ArmIR* a, LLVMValueRef x) { return LLVMBuildZExt(a->b, x, a->i32, ""); }
static LLVMValueRef z64(ArmIR* a, LLVMValueRef x) { return LLVMBuildZExt(a->b, x, a->i64, ""); }
static LLVMValueRef field(ArmIR* a, size_t offset) {
    LLVMValueRef i = c64(a, offset);
    return LLVMBuildGEP2(a->b, a->i8, a->cpu, &i, 1, "");
}
static LLVMValueRef get(ArmIR* a, size_t offset, LLVMTypeRef t) {
    return LLVMBuildLoad2(a->b, t, field(a, offset), "");
}
static void put(ArmIR* a, size_t offset, LLVMValueRef v) { LLVMBuildStore(a->b, v, field(a, offset)); }
#define GET(a, f)    get(a, offsetof(SnxArmCPU, f), (a)->i32)
#define PUT(a, f, v) put(a, offsetof(SnxArmCPU, f), v)
static LLVMValueRef reg(ArmIR* a, unsigned r) {
    return r == 15 ? c32(a, a->thumb ? (a->pc + 4) & ~3u : a->pc + 8) : get(a, offsetof(SnxArmCPU, r) + r * 4, a->i32);
}
static void setreg(ArmIR* a, unsigned r, LLVMValueRef v) { put(a, offsetof(SnxArmCPU, r) + r * 4, v); }
static LLVMValueRef flag(ArmIR* a, unsigned n) { return band(a, lsr(a, GET(a, cpsr), n), c32(a, 1)); }
static LLVMBasicBlockRef block(ArmIR* a, const char* name) {
    return LLVMAppendBasicBlockInContext(a->ctx, a->fn, name);
}
/* void name(SnxArmCPU*, u32...): a call into the runtime library (rt_helpers.c), which may change any of the state */
static void rt_call(ArmIR* a, const char* name, LLVMValueRef* args, unsigned count) {
    LLVMTypeRef params[3] = {a->ptr, a->i32, a->i32};
    LLVMTypeRef type = LLVMFunctionType(LLVMVoidTypeInContext(a->ctx), params, count + 1, 0);
    LLVMValueRef fn = LLVMGetNamedFunction(a->mod, name), all[3];
    unsigned i;
    if (!fn) fn = LLVMAddFunction(a->mod, name, type);
    all[0] = a->cpu;
    for (i = 0; i < count; ++i) all[i + 1] = args[i];
    LLVMBuildCall2(a->b, type, fn, all, count + 1, "");
}
/* after a runtime library call: the block ends here when it set an exit */
static void stop_on_exit(ArmIR* a) {
    LLVMBasicBlockRef go = block(a, "go"), stop = block(a, "stop");
    LLVMBuildCondBr(a->b, LLVMBuildICmp(a->b, LLVMIntEQ, GET(a, exit), c32(a, SNX_ARM_OK), ""), go, stop);
    LLVMPositionBuilderAtEnd(a->b, stop);
    LLVMBuildRetVoid(a->b);
    LLVMPositionBuilderAtEnd(a->b, go);
}
/* Ends the block with an exit. An instruction the block can't run (UNDEFINED, BACKEND_UNSUPPORTED) is the backend's
   to decide instead (SnxArmRt.undefined): the block ends at it when it sets an exit, or after it when it ran it */
static void trap(ArmIR* a, SnxArmExit code, LLVMValueRef syndrome) {
    if (code == SNX_ARM_UNDEFINED || code == SNX_ARM_BACKEND_UNSUPPORTED) {
        LLVMValueRef args[2];
        args[0] = syndrome;
        args[1] = c32(a, code);
        rt_call(a, "snx_arm_undefined", args, 2);
        stop_on_exit(a);
        setreg(a, 15, c32(a, a->next));
        LLVMBuildRetVoid(a->b);
        return;
    }
    PUT(a, exit, c32(a, code));
    PUT(a, syndrome, syndrome);
    LLVMBuildRetVoid(a->b);
}
/* Continue only when valid. Fault paths do not reach host memory or write back. */
static void require(ArmIR* a, LLVMValueRef valid, SnxArmExit code, LLVMValueRef syndrome) {
    LLVMBasicBlockRef ok = block(a, "ok"), bad = block(a, "fault");
    LLVMBuildCondBr(a->b, valid, ok, bad);
    LLVMPositionBuilderAtEnd(a->b, bad);
    trap(a, code, syndrome);
    LLVMPositionBuilderAtEnd(a->b, ok);
}
static LLVMValueRef condition(ArmIR* a, unsigned cond) {
    LLVMValueRef n = flag(a, 31), z = flag(a, 30), c = flag(a, 29), v = flag(a, 28), r;
    switch (cond >> 1) {
        case 0: r = z; break;
        case 1: r = c; break;
        case 2: r = n; break;
        case 3: r = v; break;
        case 4: r = band(a, c, bxor(a, z, c32(a, 1))); break;
        case 5: r = z32(a, cmp(a, LLVMIntEQ, n, v)); break;
        case 6: r = band(a, bxor(a, z, c32(a, 1)), z32(a, cmp(a, LLVMIntEQ, n, v))); break;
        default: return LLVMConstInt(a->i1, 1, 0);
    }
    return cmp(a, (cond & 1) ? LLVMIntEQ : LLVMIntNE, r, c32(a, 0));
}
static void nz(ArmIR* a, LLVMValueRef value, LLVMValueRef carry, LLVMValueRef overflow) {
    LLVMValueRef f = GET(a, cpsr);
    uint32_t mask = 0xc0000000u | (carry ? 0x20000000u : 0) | (overflow ? 0x10000000u : 0);
    f = band(a, f, c32(a, ~mask));
    f = bor(a, f, band(a, value, c32(a, 0x80000000u)));
    f = bor(a, f, shl(a, z32(a, cmp(a, LLVMIntEQ, value, c32(a, 0))), 30));
    if (carry) f = bor(a, f, shl(a, carry, 29));
    if (overflow) f = bor(a, f, shl(a, overflow, 28));
    PUT(a, cpsr, f);
}
static LLVMValueRef ror(ArmIR* a, LLVMValueRef v, unsigned n) {
    return n ? bor(a, lsr(a, v, n), shl(a, v, 32 - n)) : v;
}
/* Shift_C, including register amounts 0,32,>32 and RRX. Mask every dynamic
   LLVM shift even in unselected arms: oversized shifts otherwise make poison. */
static LLVMValueRef shift(ArmIR* a, LLVMValueRef v, unsigned type, LLVMValueRef amount, int immediate, unsigned imm,
                          LLVMValueRef* carry) {
    LLVMValueRef old = flag(a, 29), result, c;
    if (immediate) {
        if (!imm && type == 0) {
            *carry = old;
            return v;
        }
        if (!imm && type == 3) {
            *carry = band(a, v, c32(a, 1));
            return bor(a, shl(a, old, 31), lsr(a, v, 1));
        }
        if (!imm) imm = 32;
        if (type == 0) {
            c = band(a, lsr(a, v, 32 - imm), c32(a, 1));
            result = shl(a, v, imm);
        } else if (type == 1) {
            c = band(a, lsr(a, v, imm - 1), c32(a, 1));
            result = imm == 32 ? c32(a, 0) : lsr(a, v, imm);
        } else if (type == 2) {
            c = band(a, lsr(a, v, imm - 1), c32(a, 1));
            result = LLVMBuildAShr(a->b, v, c32(a, imm == 32 ? 31 : imm), "");
        } else {
            result = ror(a, v, imm);
            c = lsr(a, result, 31);
        }
    } else {
        LLVMValueRef k = band(a, amount, c32(a, 255));
        LLVMValueRef k5 = band(a, k, c32(a, 31));
        LLVMValueRef zero = cmp(a, LLVMIntEQ, k, c32(a, 0));
        LLVMValueRef small = cmp(a, LLVMIntULT, k, c32(a, 32));
        LLVMValueRef atmost = cmp(a, LLVMIntULE, k, c32(a, 32));
        LLVMValueRef km1 = band(a, sub(a, k, c32(a, 1)), c32(a, 31));
        LLVMValueRef negk = band(a, sub(a, c32(a, 0), k), c32(a, 31));
        if (type == 0) {
            result = choose(a, small, LLVMBuildShl(a->b, v, k5, ""), c32(a, 0));
            c = choose(a, atmost, band(a, LLVMBuildLShr(a->b, v, negk, ""), c32(a, 1)), c32(a, 0));
        } else if (type == 1) {
            result = choose(a, small, LLVMBuildLShr(a->b, v, k5, ""), c32(a, 0));
            c = choose(a, atmost, band(a, LLVMBuildLShr(a->b, v, km1, ""), c32(a, 1)), c32(a, 0));
        } else if (type == 2) {
            result = LLVMBuildAShr(a->b, v, choose(a, small, k5, c32(a, 31)), "");
            c = choose(a, small, band(a, LLVMBuildLShr(a->b, v, km1, ""), c32(a, 1)), lsr(a, v, 31));
        } else {
            result = bor(a, LLVMBuildLShr(a->b, v, k5, ""), LLVMBuildShl(a->b, v, negk, ""));
            c = lsr(a, result, 31);
        }
        result = choose(a, zero, v, result);
        c = choose(a, zero, old, c);
    }
    *carry = c;
    return result;
}
static LLVMValueRef operand2(ArmIR* a, uint32_t w, LLVMValueRef* carry) {
    if (w & (1u << 25)) {
        unsigned rot = ((w >> 8) & 15) * 2;
        uint32_t x = w & 255;
        if (rot) x = (x >> rot) | (x << (32 - rot));
        *carry = rot ? c32(a, x >> 31) : flag(a, 29);
        return c32(a, x);
    }
    return shift(a, reg(a, w & 15), (w >> 5) & 3, reg(a, (w >> 8) & 15), !(w & 16), (w >> 7) & 31, carry);
}
static void branch_exchange(ArmIR* a, LLVMValueRef target) {
    LLVMValueRef thumb = band(a, target, c32(a, 1));
    /* No alignment check here: a misaligned ARM target faults at the next
       instruction fetch (the dispatcher's job), not at the branch itself. */
    PUT(a, cpsr, bor(a, band(a, GET(a, cpsr), c32(a, ~SNX_ARM_T)), shl(a, thumb, 5)));
    setreg(a, 15, band(a, target, c32(a, ~1u)));
}
static LLVMValueRef arithmetic(ArmIR* a, LLVMValueRef x, LLVMValueRef y, LLVMValueRef cin, int flags) {
    LLVMValueRef wide = add(a, add(a, z64(a, x), z64(a, y)), z64(a, cin));
    LLVMValueRef result = LLVMBuildTrunc(a->b, wide, a->i32, "");
    if (flags) {
        LLVMValueRef c = LLVMBuildTrunc(a->b, LLVMBuildLShr(a->b, wide, c64(a, 32), ""), a->i32, "");
        LLVMValueRef v = lsr(a, band(a, LLVMBuildNot(a->b, bxor(a, x, y), ""), bxor(a, x, result)), 31);
        nz(a, result, c, v);
    }
    return result;
}
static void data(ArmIR* a) {
    uint32_t w = a->word;
    unsigned op = (w >> 21) & 15, d = (w >> 12) & 15;
    int flags = !!(w & (1u << 20)), logic = 0;
    LLVMValueRef carry, x = reg(a, (w >> 16) & 15), y = operand2(a, w, &carry), r;
    switch (op) {
        case 0:
        case 8:
            r = band(a, x, y);
            logic = 1;
            break;
        case 1:
        case 9:
            r = bxor(a, x, y);
            logic = 1;
            break;
        case 2:
        case 10: r = arithmetic(a, x, LLVMBuildNot(a->b, y, ""), c32(a, 1), flags); break;
        case 3: r = arithmetic(a, y, LLVMBuildNot(a->b, x, ""), c32(a, 1), flags); break;
        case 4:
        case 11: r = arithmetic(a, x, y, c32(a, 0), flags); break;
        case 5: r = arithmetic(a, x, y, flag(a, 29), flags); break;
        case 6: r = arithmetic(a, x, LLVMBuildNot(a->b, y, ""), flag(a, 29), flags); break;
        case 7: r = arithmetic(a, y, LLVMBuildNot(a->b, x, ""), flag(a, 29), flags); break;
        case 12:
            r = bor(a, x, y);
            logic = 1;
            break;
        case 13:
            r = y;
            logic = 1;
            break;
        case 14:
            r = band(a, x, LLVMBuildNot(a->b, y, ""));
            logic = 1;
            break;
        default:
            r = LLVMBuildNot(a->b, y, "");
            logic = 1;
            break;
    }
    if (flags && logic) nz(a, r, carry, NULL);
    if (op >= 8 && op <= 11) return;
    if (d == 15) {
        if (a->profile == SNX_ARM_PROFILE_ARM11)
            setreg(a, 15, band(a, r, c32(a, ~3u)));
        else
            branch_exchange(a, r);
    } else
        setreg(a, d, r);
}

static void bounds(ArmIR* a, LLVMValueRef addr, unsigned bytes, unsigned alignment) {
    LLVMValueRef end, size;
    if (alignment > 1)
        require(a, cmp(a, LLVMIntEQ, band(a, addr, c32(a, alignment - 1)), c32(a, 0)), SNX_ARM_ALIGNMENT, addr);
    end = add(a, z64(a, addr), c64(a, bytes));
    size = get(a, offsetof(SnxArmCPU, mem_size), a->i64);
    /* the AGB board's window runs past 4 GiB by a guard (agb/cpu/memmap.c): an access the console's bus wraps, an LDM
       from 0xFFFFFFE8, lands in it instead of aborting */
    if (arm7tdmi(a))
        require(a, cmp(a, LLVMIntULE, end, size), SNX_ARM_DATA_ABORT, addr);
    else
        require(a, band(a, cmp(a, LLVMIntULE, end, size), cmp(a, LLVMIntULE, end, c64(a, 1ull << 32))),
                SNX_ARM_DATA_ABORT, addr);
}
/* Checked callbacks never see host guest pointers, only a bounded transaction
   buffer. Their faults precede register/base writeback, even for LDM/STM. */
static LLVMValueRef bus_operation(ArmIR* a, unsigned operation, LLVMValueRef addr, unsigned bytes,
                                  LLVMValueRef buffer) {
    LLVMTypeRef types[] = {a->ptr, a->i32, a->i32, a->i32, a->ptr};
    LLVMTypeRef ft = LLVMFunctionType(a->i32, types, 5, 0);
    LLVMValueRef fn = get(a, offsetof(SnxArmCheckedCPU, access), a->ptr);
    LLVMValueRef args[] = {get(a, offsetof(SnxArmCheckedCPU, context), a->ptr), c32(a, operation), addr, c32(a, bytes),
                           buffer};
    return LLVMBuildCall2(a->b, ft, fn, args, 5, "bus.status");
}
static void bus_access(ArmIR* a, unsigned operation, LLVMValueRef addr, unsigned bytes, LLVMValueRef buffer) {
    LLVMValueRef result = bus_operation(a, operation, addr, bytes, buffer);
    require(a, cmp(a, LLVMIntEQ, result, c32(a, 0)),
            operation == SNX_ARM_ACCESS_FETCH ? SNX_ARM_PREFETCH_ABORT : SNX_ARM_DATA_ABORT, addr);
}
static void memory_entry(ArmIR* a) {
    if (a->memory == SNX_ARM_MEMORY_FLAT) {
        a->mem = get(a, offsetof(SnxArmCPU, mem), a->ptr);
        return;
    }
    require(a, cmp(a, LLVMIntEQ, get(a, offsetof(SnxArmCheckedCPU, abi), a->i32), c32(a, SNX_ARM_CHECKED_ABI)),
            SNX_ARM_BAD_ENTRY, c32(a, SNX_ARM_CHECKED_ABI));
    require(a, cmp(a, LLVMIntNE, get(a, offsetof(SnxArmCheckedCPU, access), a->ptr), LLVMConstNull(a->ptr)),
            SNX_ARM_BAD_ENTRY, c32(a, SNX_ARM_CHECKED_ABI));
}
static void instruction_fetch(ArmIR* a, uint32_t encoded, unsigned bytes) {
    if (a->memory != SNX_ARM_MEMORY_CHECKED) return;
    LLVMTypeRef type = LLVMIntTypeInContext(a->ctx, bytes * 8);
    LLVMValueRef buffer = LLVMBuildAlloca(a->b, type, "fetch.bytes");
    bus_access(a, SNX_ARM_ACCESS_FETCH, c32(a, a->pc), bytes, buffer);
    LLVMValueRef value = LLVMBuildLoad2(a->b, type, buffer, "");
    value = host_order(a, value, bytes);
    require(a, cmp(a, LLVMIntEQ, value, LLVMConstInt(type, encoded, 0)), SNX_ARM_CODE_CHANGED, c32(a, a->pc));
}
static void transfer_begin(ArmIR* a, LLVMValueRef addr, unsigned bytes, unsigned alignment, int write, int multi) {
    if (a->memory == SNX_ARM_MEMORY_FLAT) {
        bounds(a, addr, bytes, alignment);
        return;
    }
    if (alignment > 1)
        require(a, cmp(a, LLVMIntEQ, band(a, addr, c32(a, alignment - 1)), c32(a, 0)), SNX_ARM_ALIGNMENT, addr);
    require(a, cmp(a, LLVMIntULE, add(a, z64(a, addr), c64(a, bytes)), c64(a, 1ull << 32)), SNX_ARM_DATA_ABORT,
            addr);
    a->transfer_base = addr;
    a->transfer_bytes = bytes;
    a->transfer_operation = (write ? SNX_ARM_ACCESS_WRITE : SNX_ARM_ACCESS_READ) | (multi ? SNX_ARM_ACCESS_MULTI : 0);
    a->transfer_buffer = LLVMBuildAlloca(a->b, LLVMArrayType(a->i8, bytes), "transfer.bytes");
    if (!write) bus_access(a, a->transfer_operation, addr, bytes, a->transfer_buffer);
}
static void transfer_commit(ArmIR* a) {
    if (a->memory == SNX_ARM_MEMORY_CHECKED)
        bus_access(a, a->transfer_operation, a->transfer_base, a->transfer_bytes, a->transfer_buffer);
}
static LLVMValueRef memptr(ArmIR* a, LLVMValueRef addr) {
    LLVMValueRef offset = z64(a, a->memory == SNX_ARM_MEMORY_CHECKED ? sub(a, addr, a->transfer_base) : addr);
    return LLVMBuildGEP2(a->b, a->i8, a->memory == SNX_ARM_MEMORY_CHECKED ? a->transfer_buffer : a->mem, &offset, 1,
                         "");
}
/* The bus hook (SnxArmCPU.io): the BIOS, the I/O window, the palette, VRAM, OAM and the save memory have
   bus rules a plain access cannot follow (the BIOS's open bus, mirrors finer than a page, byte write
   widths). The runtime registers a hook the way the A64 side registers a fetch callback; without one every
   access is direct. The regions are the AGB board's: 0x00 to 0x01 the BIOS, 0x04 to 0x07 the I/O
   window and video memory, 0x0E and 0x0F the save memory. */
#define SNX_ARM_IO_REGIONS (1u << 0x00 | 1u << 0x01 | 1u << 0x04 | 1u << 0x05 | 1u << 0x06 | 1u << 0x07 | \
                            1u << 0x0E | 1u << 0x0F)
static LLVMValueRef io_route(ArmIR* a, LLVMValueRef addr, LLVMValueRef fn) {
    LLVMValueRef top = lsr(a, addr, 24);
    LLVMValueRef route = band(a, LLVMBuildLShr(a->b, c32(a, SNX_ARM_IO_REGIONS), band(a, top, c32(a, 31)), ""),
                              z32(a, cmp(a, LLVMIntULT, top, c32(a, 32))));
    route = band(a, route, c32(a, 1));
    return band(a, route, z32(a, cmp(a, LLVMIntNE, fn, LLVMConstNull(a->ptr))));
}
static LLVMTypeRef io_type(ArmIR* a) {
    LLVMTypeRef params[4] = {a->ptr, a->i32, a->i32, a->i32};
    return LLVMFunctionType(a->i32, params, 4, 0);
}
/* The GBA's ARM7TDMI rotates a misaligned load's aligned data into place in the 32 bit register instead
   of reading across the boundary (ARMv8-A would read the unaligned bytes directly). The zero
   misalignment selects the value unchanged through the shift arithmetic itself. */
static LLVMValueRef rotated_load(ArmIR* a, LLVMValueRef value, LLVMValueRef addr, unsigned bytes) {
    LLVMValueRef mis = band(a, addr, c32(a, bytes - 1));
    LLVMValueRef amount = shl(a, mis, 3);
    LLVMValueRef top = LLVMBuildLShr(a->b, value, amount, "");
    LLVMValueRef bottom = LLVMBuildShl(a->b, value, band(a, sub(a, c32(a, 32), amount), c32(a, 31)), "");
    return bor(a, top, bottom);
}
/* Normal memory permits unaligned single-register transfers. Do not give LLVM
   its default ABI alignment, and do not split aligned words into byte stores. */
static LLVMValueRef readmem(ArmIR* a, LLVMValueRef addr, unsigned bytes, int sign) {
    LLVMTypeRef type = LLVMIntTypeInContext(a->ctx, bytes * 8);
    if (!arm7tdmi(a)) {
        LLVMValueRef value = LLVMBuildLoad2(a->b, type, memptr(a, addr), "");
        LLVMSetAlignment(value, 1);
        value = host_order(a, value, bytes);
        if (bytes < 4) value = sign ? LLVMBuildSExt(a->b, value, a->i32, "") : z32(a, value);
        return value;
    }
    LLVMValueRef fn = get(a, offsetof(SnxArmCPU, io), a->ptr);
    LLVMValueRef aligned = band(a, addr, c32(a, ~(uint32_t)(bytes - 1)));
    LLVMBasicBlockRef io = block(a, "io"), mem = block(a, "mem"), done = block(a, "read");
    LLVMValueRef incoming[2], joined;
    LLVMBasicBlockRef from[2];
    LLVMBuildCondBr(a->b, cmp(a, LLVMIntNE, io_route(a, addr, fn), c32(a, 0)), io, mem);
    LLVMPositionBuilderAtEnd(a->b, mem);
    {
        LLVMValueRef value = LLVMBuildLoad2(a->b, type, memptr(a, aligned), "");
        LLVMSetAlignment(value, 1);
        value = host_order(a, value, bytes);
        if (sign && bytes == 2) {
            /* ARMv4T: a misaligned LDRSH reads a sign extended byte instead */
            LLVMValueRef wide = LLVMBuildSExt(a->b, value, a->i32, "");
            LLVMValueRef narrow =
                LLVMBuildSExt(a->b, LLVMBuildLoad2(a->b, a->i8, memptr(a, addr), ""), a->i32, "");
            incoming[0] = LLVMBuildSelect(a->b, cmp(a, LLVMIntNE, band(a, addr, c32(a, 1)), c32(a, 0)), narrow, wide,
                                         "");
        } else
            incoming[0] = bytes < 4 ? z32(a, value) : value;
        from[0] = mem;
    }
    LLVMBuildBr(a->b, done);
    LLVMPositionBuilderAtEnd(a->b, io);
    {
        LLVMValueRef args[4] = {a->cpu, aligned, c32(a, (sign && bytes == 2 ? 0x200 : 0) | bytes), c32(a, 0)};
        incoming[1] = LLVMBuildCall2(a->b, io_type(a), fn, args, 4, "");
        from[1] = io;
    }
    LLVMBuildBr(a->b, done);
    LLVMPositionBuilderAtEnd(a->b, done);
    joined = LLVMBuildPhi(a->b, a->i32, "");
    LLVMAddIncoming(joined, incoming, from, 2);
    if (sign && bytes == 2) return joined; /* the quirk value is already the result */
    /* both paths carry the aligned data zero extended; a signed load narrows back and widens again */
    if (bytes > 1) joined = rotated_load(a, joined, addr, bytes);
    return sign && bytes < 4 ? LLVMBuildSExt(a->b, LLVMBuildTrunc(a->b, joined, type, ""), a->i32, "") : joined;
}
static void writemem(ArmIR* a, LLVMValueRef addr, unsigned bytes, LLVMValueRef value) {
    LLVMTypeRef type = LLVMIntTypeInContext(a->ctx, bytes * 8);
    if (!arm7tdmi(a)) {
        LLVMValueRef store;
        if (bytes < 4) value = LLVMBuildTrunc(a->b, value, type, "");
        store = LLVMBuildStore(a->b, host_order(a, value, bytes), memptr(a, addr));
        LLVMSetAlignment(store, 1);
        /* A local ordinary store may clear the monitor. Clearing on every local
           store is a permitted spurious exclusive failure. */
        PUT(a, exclusive_valid, c32(a, 0));
        return;
    }
    LLVMValueRef fn = get(a, offsetof(SnxArmCPU, io), a->ptr);
    LLVMBasicBlockRef io = block(a, "io"), mem = block(a, "mem"), done = block(a, "written");
    PUT(a, exclusive_valid, c32(a, 0));
    LLVMBuildCondBr(a->b, cmp(a, LLVMIntNE, io_route(a, addr, fn), c32(a, 0)), io, mem);
    LLVMPositionBuilderAtEnd(a->b, mem);
    {
        LLVMValueRef narrow = bytes < 4 ? LLVMBuildTrunc(a->b, value, type, "") : value;
        /* a misaligned store lands in the aligned unit on the GBA, without the load rotation */
        LLVMValueRef store =
            LLVMBuildStore(a->b, host_order(a, narrow, bytes), memptr(a, band(a, addr, c32(a, ~(uint32_t)(bytes - 1)))));
        LLVMSetAlignment(store, 1);
    }
    LLVMBuildBr(a->b, done);
    LLVMPositionBuilderAtEnd(a->b, io);
    {
        /* the hook sees the raw address: the save memory's 8 bit bus picks the lane from it */
        LLVMValueRef args[4] = {a->cpu, addr, c32(a, 0x100 | bytes), value};
        LLVMBuildCall2(a->b, io_type(a), fn, args, 4, "");
    }
    LLVMBuildBr(a->b, done);
    LLVMPositionBuilderAtEnd(a->b, done);
}
/* LDM and STM move raw words at the exact addresses: the GBA's single transfer rotation and
   alignment do not apply to the list transfers. The bus hook still sees them. */
static LLVMValueRef readmem_list(ArmIR* a, LLVMValueRef addr, unsigned bytes) {
    if (!arm7tdmi(a)) return readmem(a, addr, bytes, 0);
    LLVMTypeRef type = LLVMIntTypeInContext(a->ctx, bytes * 8);
    LLVMValueRef fn = get(a, offsetof(SnxArmCPU, io), a->ptr);
    LLVMBasicBlockRef io = block(a, "io"), mem = block(a, "mem"), done = block(a, "read");
    LLVMValueRef incoming[2], joined;
    LLVMBasicBlockRef from[2];
    LLVMBuildCondBr(a->b, cmp(a, LLVMIntNE, io_route(a, addr, fn), c32(a, 0)), io, mem);
    LLVMPositionBuilderAtEnd(a->b, mem);
    {
        LLVMValueRef value = LLVMBuildLoad2(a->b, type, memptr(a, addr), "");
        LLVMSetAlignment(value, 1);
        incoming[0] = host_order(a, value, bytes);
        from[0] = mem;
    }
    LLVMBuildBr(a->b, done);
    LLVMPositionBuilderAtEnd(a->b, io);
    {
        LLVMValueRef args[4] = {a->cpu, addr, c32(a, bytes), c32(a, 0)};
        incoming[1] = LLVMBuildCall2(a->b, io_type(a), fn, args, 4, "");
        from[1] = io;
    }
    LLVMBuildBr(a->b, done);
    LLVMPositionBuilderAtEnd(a->b, done);
    joined = LLVMBuildPhi(a->b, a->i32, "");
    LLVMAddIncoming(joined, incoming, from, 2);
    return bytes < 4 ? z32(a, joined) : joined;
}
static void writemem_list(ArmIR* a, LLVMValueRef addr, unsigned bytes, LLVMValueRef value) {
    if (!arm7tdmi(a)) {
        writemem(a, addr, bytes, value);
        return;
    }
    LLVMTypeRef type = LLVMIntTypeInContext(a->ctx, bytes * 8);
    LLVMValueRef fn = get(a, offsetof(SnxArmCPU, io), a->ptr);
    LLVMBasicBlockRef io = block(a, "io"), mem = block(a, "mem"), done = block(a, "written");
    LLVMBuildCondBr(a->b, cmp(a, LLVMIntNE, io_route(a, addr, fn), c32(a, 0)), io, mem);
    LLVMPositionBuilderAtEnd(a->b, mem);
    {
        LLVMValueRef narrow = bytes < 4 ? LLVMBuildTrunc(a->b, value, type, "") : value;
        LLVMValueRef store = LLVMBuildStore(a->b, host_order(a, narrow, bytes), memptr(a, addr));
        LLVMSetAlignment(store, 1);
    }
    LLVMBuildBr(a->b, done);
    LLVMPositionBuilderAtEnd(a->b, io);
    {
        LLVMValueRef args[4] = {a->cpu, addr, c32(a, 0x100 | bytes), value};
        LLVMBuildCall2(a->b, io_type(a), fn, args, 4, "");
    }
    LLVMBuildBr(a->b, done);
    LLVMPositionBuilderAtEnd(a->b, done);
}
static void memory(ArmIR* a, SnxA32Kind kind) {
    uint32_t w = a->word;
    unsigned n = (w >> 16) & 15, d = (w >> 12) & 15, op = (w >> 5) & 3;
    int pre = !!(w & (1u << 24)), up = !!(w & (1u << 23));
    int wb = !pre || !!(w & (1u << 21)), load = !!(w & (1u << 20));
    unsigned bytes = 4;
    int sign = 0, pair = 0;
    LLVMValueRef base = reg(a, n), offset, adjusted, addr, value, second = NULL, unused;
    if (kind == SNX_A32_SINGLE) {
        bytes = w & (1u << 22) ? 1 : 4;
        offset =
            w & (1u << 25) ? shift(a, reg(a, w & 15), (w >> 5) & 3, NULL, 1, (w >> 7) & 31, &unused) : c32(a, w & 4095);
    } else {
        offset = w & (1u << 22) ? c32(a, ((w >> 4) & 0xf0) | (w & 15)) : reg(a, w & 15);
        if (!load && op >= 2) {
            pair = 1;
            load = op == 2;
        } else {
            bytes = op == 2 ? 1 : 2;
            sign = op >= 2;
        }
    }
    adjusted = up ? add(a, base, offset) : sub(a, base, offset);
    addr = pre ? adjusted : base;
    /* With unaligned support on (ARMv6 SCTLR.U, the model ordinary transfers follow), LDRD and STRD need word
       alignment only. Doubleword alignment is the legacy U=0 rule, and libctru's file code loads 64-bit offsets
       from word-aligned structures */
    unsigned alignment = pair || (load && d == 15) ? 4 : 1;
    transfer_begin(a, addr, pair ? 8 : bytes, alignment, !load, pair);
    if (load) {
        value = readmem(a, addr, bytes, sign);
        if (pair) second = readmem(a, add(a, addr, c32(a, 4)), 4, 0);
        /* Check a loaded PC before committing writeback. */
        if (d == 15)
            branch_exchange(a, value);
        else
            setreg(a, d, value);
        if (pair) setreg(a, d + 1, second);
    } else {
        value = reg(a, d); /* Cortex-A57's PCStoreValue is instruction address + 8, the ARM7TDMI's + 12 */
        if (d == 15 && arm7tdmi(a)) value = c32(a, a->pc + 12);
        if (pair) second = reg(a, d + 1);
        writemem(a, addr, bytes, value);
        if (pair) writemem(a, add(a, addr, c32(a, 4)), 4, second);
        transfer_commit(a);
    }
    if (wb) setreg(a, n, adjusted);
}
static void multiple(ArmIR* a) {
    uint32_t w = a->word;
    unsigned n = (w >> 16) & 15, count = 0, i, index = 0;
    int pre = !!(w & (1u << 24)), up = !!(w & (1u << 23)), load = !!(w & (1u << 20));
    int quirks = arm7tdmi(a);
    LLVMValueRef base = reg(a, n), start, adjusted, values[16] = {0};
    for (i = 0; i < 16; ++i) count += (w >> i) & 1;
    adjusted = up ? add(a, base, c32(a, count * 4)) : sub(a, base, c32(a, count * 4));
    start = up ? add(a, base, c32(a, pre ? 4 : 0)) : add(a, adjusted, c32(a, pre ? 0 : 4));
    /* The GBA's ARM7TDMI transfers at an unaligned base instead of faulting */
    transfer_begin(a, start, count * 4, quirks ? 1 : 4, !load, 1);
    /* Snapshot all sources before updates, particularly SP and the PC. */
    for (i = 0; i < 16; ++i)
        if (w & (1u << i)) {
            /* the ARM7TDMI stores the written back base when the base is not the first in the list */
            int first = !(w & ((1u << i) - 1));
            values[i] = load      ? readmem_list(a, add(a, start, c32(a, index * 4)), 4)
                        : !quirks ? reg(a, i)
                                  : (i == 15  ? c32(a, a->pc + 12)
                                     : i == n ? ((w & (1u << 21)) && !first ? adjusted : reg(a, i))
                                              : reg(a, i));
            ++index;
        }
    if (load && values[15]) branch_exchange(a, values[15]);
    index = 0;
    for (i = 0; i < 16; ++i)
        if (values[i]) {
            if (load) {
                if (i != 15) setreg(a, i, values[i]);
            } else
                writemem_list(a, add(a, start, c32(a, index * 4)), 4, values[i]);
            ++index;
        }
    if (!load) transfer_commit(a);
    /* Writeback after stores (STM snapshots keep the old base) but before
       nothing else: for LDM with the base in the list the loaded value wins,
       so writeback is discarded there. */
    if ((w & (1u << 21)) && !(load && (w & (1u << n)))) setreg(a, n, adjusted);
}

static LLVMValueRef intrinsic(ArmIR* a, const char* name, size_t len, LLVMTypeRef type, LLVMValueRef* args,
                              unsigned count) {
    unsigned id = LLVMLookupIntrinsicID(name, len);
    LLVMValueRef fn = LLVMGetIntrinsicDeclaration(a->mod, id, &type, 1);
    return LLVMBuildCall2(a->b, LLVMGlobalGetValueType(fn), fn, args, count, "");
}
/* a value the guest's little-endian memory holds, as the target's number: swapped for a big-endian target, which the
   module's data layout says (the code is the target's, which a recompiler may build for another machine) */
static LLVMValueRef host_order(ArmIR* a, LLVMValueRef v, unsigned bytes) {
    if (bytes > 1 && LLVMByteOrder(LLVMGetModuleDataLayout(a->mod)) == LLVMBigEndian)
        return intrinsic(a, "llvm.bswap", 10, LLVMTypeOf(v), &v, 1);
    return v;
}
/* ABI-1 optional RAM reservation operations. The bus owns the write epoch,
   so aliases, DMA/kernel writes and remaps cannot pass an ABA value check.
   CPU-local address/size/valid are additional guards, never the global monitor.
   Only the explicit ARM11+checked path uses this extension. */
static void checked_exclusive(ArmIR* a) {
    uint32_t w = a->word;
    unsigned op = (w >> 20) & 7, n = (w >> 16) & 15, d = (w >> 12) & 15, t = w & 15;
    static const unsigned widths[] = {4, 8, 1, 2};
    unsigned bytes = widths[op >> 1];
    LLVMTypeRef type = LLVMIntTypeInContext(a->ctx, bytes * 8);
    LLVMValueRef addr = reg(a, n), value, buffer = LLVMBuildAlloca(a->b, type, "exclusive.bytes");
    require(a, cmp(a, LLVMIntEQ, band(a, addr, c32(a, bytes - 1)), c32(a, 0)), SNX_ARM_ALIGNMENT, addr);
    if (op & 1) {
        bus_access(a, SNX_ARM_ACCESS_READ | SNX_ARM_ACCESS_EXCLUSIVE, addr, bytes, buffer);
        value = host_order(a, LLVMBuildLoad2(a->b, type, buffer, ""), bytes);
        if (bytes < 8) value = LLVMBuildZExt(a->b, value, a->i64, "");
        put(a, offsetof(SnxArmCPU, exclusive_value), value);
        PUT(a, exclusive_address, addr);
        PUT(a, exclusive_size, c32(a, bytes));
        PUT(a, exclusive_valid, c32(a, 1));
        setreg(a, d, LLVMBuildTrunc(a->b, value, a->i32, ""));
        if (bytes == 8) setreg(a, d + 1, LLVMBuildTrunc(a->b, LLVMBuildLShr(a->b, value, c64(a, 32), ""), a->i32, ""));
    } else {
        LLVMBasicBlockRef attempt = block(a, "exclusive.try"), fail = block(a, "exclusive.fail"),
                          join = block(a, "exclusive.end");
        LLVMValueRef valid = cmp(a, LLVMIntNE, GET(a, exclusive_valid), c32(a, 0));
        valid = band(a, valid, cmp(a, LLVMIntEQ, GET(a, exclusive_address), addr));
        valid = band(a, valid, cmp(a, LLVMIntEQ, GET(a, exclusive_size), c32(a, bytes)));
        PUT(a, exclusive_valid, c32(a, 0));
        LLVMBuildCondBr(a->b, valid, attempt, fail);
        LLVMPositionBuilderAtEnd(a->b, attempt);
        value = reg(a, t);
        if (bytes == 8)
            value = bor(a, z64(a, value), LLVMBuildShl(a->b, z64(a, reg(a, t + 1)), c64(a, 32), ""));
        else if (bytes < 4)
            value = LLVMBuildTrunc(a->b, value, type, "");
        LLVMBuildStore(a->b, host_order(a, value, bytes), buffer);
        LLVMValueRef status = bus_operation(a, SNX_ARM_ACCESS_WRITE | SNX_ARM_ACCESS_EXCLUSIVE, addr, bytes, buffer);
        require(
            a,
            bor(a, cmp(a, LLVMIntEQ, status, c32(a, 0)), cmp(a, LLVMIntEQ, status, c32(a, SNX_ARM_EXCLUSIVE_FAILED))),
            SNX_ARM_DATA_ABORT, addr);
        LLVMValueRef result = z32(a, cmp(a, LLVMIntNE, status, c32(a, 0)));
        LLVMBasicBlockRef from[] = {LLVMGetInsertBlock(a->b), fail};
        LLVMValueRef incoming[] = {result, c32(a, 1)};
        LLVMBuildBr(a->b, join);
        LLVMPositionBuilderAtEnd(a->b, fail);
        LLVMBuildBr(a->b, join);
        LLVMPositionBuilderAtEnd(a->b, join);
        result = LLVMBuildPhi(a->b, a->i32, "");
        LLVMAddIncoming(result, incoming, from, 2);
        setreg(a, d, result);
    }
}
static void exclusive(ArmIR* a) {
    uint32_t w = a->word;
    unsigned op = (w >> 20) & 7, n = (w >> 16) & 15, d = (w >> 12) & 15, t = w & 15;
    static const unsigned widths[] = {4, 8, 1, 2};
    unsigned bytes = widths[op >> 1];
    LLVMTypeRef type = LLVMIntTypeInContext(a->ctx, bytes * 8);
    LLVMValueRef addr = reg(a, n), p, value;
    bounds(a, addr, bytes, bytes);
    p = memptr(a, addr);
    if (op & 1) {
        value = LLVMBuildLoad2(a->b, type, p, "");
        LLVMSetOrdering(value, LLVMAtomicOrderingSequentiallyConsistent);
        LLVMSetAlignment(value, bytes);
        value = host_order(a, value, bytes);
        if (bytes < 8) value = LLVMBuildZExt(a->b, value, a->i64, "");
        put(a, offsetof(SnxArmCPU, exclusive_value), value);
        PUT(a, exclusive_address, addr);
        PUT(a, exclusive_size, c32(a, bytes));
        PUT(a, exclusive_valid, c32(a, 1));
        setreg(a, d, LLVMBuildTrunc(a->b, value, a->i32, ""));
        if (bytes == 8)
            setreg(a, a->pair ? a->pair - 1 : d + 1,
                   LLVMBuildTrunc(a->b, LLVMBuildLShr(a->b, value, c64(a, 32), ""), a->i32, ""));
    } else {
        LLVMBasicBlockRef attempt = block(a, "exclusive.try"), fail = block(a, "exclusive.fail"),
                          join = block(a, "exclusive.end");
        LLVMValueRef valid = cmp(a, LLVMIntNE, GET(a, exclusive_valid), c32(a, 0)), expected, pair, result;
        LLVMValueRef incoming[2];
        LLVMBasicBlockRef from[2];
        valid = band(a, valid, cmp(a, LLVMIntEQ, GET(a, exclusive_address), addr));
        valid = band(a, valid, cmp(a, LLVMIntEQ, GET(a, exclusive_size), c32(a, bytes)));
        PUT(a, exclusive_valid, c32(a, 0));
        LLVMBuildCondBr(a->b, valid, attempt, fail);
        LLVMPositionBuilderAtEnd(a->b, attempt);
        value = reg(a, t);
        if (bytes == 8)
            value = bor(a, z64(a, value),
                        LLVMBuildShl(a->b, z64(a, reg(a, a->pair ? a->pair - 1 : t + 1)), c64(a, 32), ""));
        else if (bytes < 4)
            value = LLVMBuildTrunc(a->b, value, type, "");
        expected = get(a, offsetof(SnxArmCPU, exclusive_value), a->i64);
        if (bytes < 8) expected = LLVMBuildTrunc(a->b, expected, type, "");
        pair = LLVMBuildAtomicCmpXchg(a->b, p, host_order(a, expected, bytes), host_order(a, value, bytes),
                                      LLVMAtomicOrderingSequentiallyConsistent,
                                      LLVMAtomicOrderingSequentiallyConsistent, 0);
        LLVMSetAlignment(pair, bytes);
        incoming[0] = bxor(a, z32(a, LLVMBuildExtractValue(a->b, pair, 1, "")), c32(a, 1));
        from[0] = LLVMGetInsertBlock(a->b);
        LLVMBuildBr(a->b, join);
        LLVMPositionBuilderAtEnd(a->b, fail);
        incoming[1] = c32(a, 1);
        from[1] = fail;
        LLVMBuildBr(a->b, join);
        LLVMPositionBuilderAtEnd(a->b, join);
        result = LLVMBuildPhi(a->b, a->i32, "");
        LLVMAddIncoming(result, incoming, from, 2);
        setreg(a, d, result);
    }
}
static void acquire_release(ArmIR* a) {
    unsigned op = (a->word >> 20) & 7, bytes = op >= 6 ? 2 : op >= 4 ? 1 : 4;
    LLVMValueRef addr = reg(a, (a->word >> 16) & 15), value, access;
    LLVMTypeRef type = LLVMIntTypeInContext(a->ctx, bytes * 8);
    bounds(a, addr, bytes, bytes);
    if (op & 1) {
        access = LLVMBuildLoad2(a->b, type, memptr(a, addr), "");
        LLVMSetOrdering(access, LLVMAtomicOrderingSequentiallyConsistent);
        LLVMSetAlignment(access, bytes);
        value = host_order(a, access, bytes);
        if (bytes < 4) value = z32(a, value);
        setreg(a, (a->word >> 12) & 15, value);
    } else {
        value = reg(a, a->word & 15);
        if (bytes < 4) value = LLVMBuildTrunc(a->b, value, type, "");
        access = LLVMBuildStore(a->b, host_order(a, value, bytes), memptr(a, addr));
        LLVMSetOrdering(access, LLVMAtomicOrderingSequentiallyConsistent);
        LLVMSetAlignment(access, bytes);
        PUT(a, exclusive_valid, c32(a, 0));
    }
}
static void multiply(ArmIR* a, SnxA32Kind kind) {
    uint32_t w = a->word;
    unsigned n = (w >> 16) & 15, d = (w >> 12) & 15;
    LLVMValueRef x = reg(a, w & 15), y = reg(a, (w >> 8) & 15), r;
    if (kind == SNX_A32_MULL && (w & 0x00e00000) == 0x00400000) { /* UMAAL: Rn * Rm + RdHi + RdLo, unsigned */
        r = add(a, LLVMBuildMul(a->b, z64(a, x), z64(a, y), ""), add(a, z64(a, reg(a, d)), z64(a, reg(a, n))));
        setreg(a, d, LLVMBuildTrunc(a->b, r, a->i32, ""));
        setreg(a, n, LLVMBuildTrunc(a->b, LLVMBuildLShr(a->b, r, c64(a, 32), ""), a->i32, ""));
    } else if (kind == SNX_A32_MULL) {
        if (w & (1u << 22)) {
            x = LLVMBuildSExt(a->b, x, a->i64, "");
            y = LLVMBuildSExt(a->b, y, a->i64, "");
        } else {
            x = z64(a, x);
            y = z64(a, y);
        }
        r = LLVMBuildMul(a->b, x, y, "");
        if (w & (1u << 21))
            r = add(a, r, bor(a, z64(a, reg(a, d)), LLVMBuildShl(a->b, z64(a, reg(a, n)), c64(a, 32), "")));
        setreg(a, d, LLVMBuildTrunc(a->b, r, a->i32, ""));
        setreg(a, n, LLVMBuildTrunc(a->b, LLVMBuildLShr(a->b, r, c64(a, 32), ""), a->i32, ""));
        if (w & (1u << 20)) {
            LLVMValueRef f = band(a, GET(a, cpsr), c32(a, 0x3fffffffu));
            LLVMValueRef sign = LLVMBuildTrunc(a->b, LLVMBuildLShr(a->b, r, c64(a, 32), ""), a->i32, "");
            f = bor(a, f, band(a, sign, c32(a, 0x80000000u)));
            f = bor(a, f, shl(a, z32(a, cmp(a, LLVMIntEQ, r, c64(a, 0))), 30));
            PUT(a, cpsr, f);
        }
    } else {
        r = LLVMBuildMul(a->b, x, y, "");
        if (kind == SNX_A32_MLS)
            r = sub(a, reg(a, d), r);
        else if (w & (1u << 21))
            r = add(a, r, reg(a, d));
        if (w & (1u << 20)) nz(a, r, NULL, NULL);
        setreg(a, n, r);
    }
}
/* ARMv5TE signed halfword multiplies (ARM11 profile, profile.c): the bottom or top half of Rm (x, bit 5) and of Rs
   (y, bit 6), sign-extended. SMULWy and SMLAWy take all of Rm and keep bits 47..16 of the 48-bit product. The
   32-bit accumulates (SMLAxy, SMLAWy) set the sticky Q flag (CPSR bit 27) on signed overflow; SMLALxy accumulates
   into RdHi:RdLo without flags */
static LLVMValueRef half(ArmIR* a, LLVMValueRef v, int top) {
    return LLVMBuildAShr(a->b, top ? v : shl(a, v, 16), c32(a, 16), "");
}
static void halfword_multiply(ArmIR* a) {
    uint32_t w = a->word;
    unsigned op = (w >> 21) & 3, d = (w >> 16) & 15, n = (w >> 12) & 15;
    LLVMValueRef m = reg(a, w & 15), y = half(a, reg(a, (w >> 8) & 15), !!(w & 0x40)), r;
    int accumulate;
    if (op == 1) {
        LLVMValueRef p = LLVMBuildMul(a->b, LLVMBuildSExt(a->b, m, a->i64, ""), LLVMBuildSExt(a->b, y, a->i64, ""), "");
        r = LLVMBuildTrunc(a->b, LLVMBuildAShr(a->b, p, c64(a, 16), ""), a->i32, "");
        accumulate = !(w & 0x20);
    } else {
        r = LLVMBuildMul(a->b, half(a, m, !!(w & 0x20)), y, ""); /* at most 0x40000000: no overflow */
        if (op == 2) {
            LLVMValueRef acc = bor(a, z64(a, reg(a, n)), LLVMBuildShl(a->b, z64(a, reg(a, d)), c64(a, 32), ""));
            acc = add(a, acc, LLVMBuildSExt(a->b, r, a->i64, ""));
            setreg(a, n, LLVMBuildTrunc(a->b, acc, a->i32, ""));
            setreg(a, d, LLVMBuildTrunc(a->b, LLVMBuildLShr(a->b, acc, c64(a, 32), ""), a->i32, ""));
            return;
        }
        accumulate = op == 0;
    }
    if (accumulate) {
        LLVMValueRef args[2] = {r, reg(a, n)};
        LLVMValueRef sum = intrinsic(a, "llvm.sadd.with.overflow", 23, a->i32, args, 2);
        LLVMValueRef overflow = z32(a, LLVMBuildExtractValue(a->b, sum, 1, ""));
        r = LLVMBuildExtractValue(a->b, sum, 0, "");
        PUT(a, cpsr, bor(a, GET(a, cpsr), shl(a, overflow, 27)));
    }
    setreg(a, d, r);
}
/* the bits lo..lo+bits-1 of x, sign- or zero-extended */
static LLVMValueRef lane(ArmIR* a, LLVMValueRef x, unsigned lo, unsigned bits, int sign) {
    if (sign) return LLVMBuildAShr(a->b, shl(a, x, 32 - lo - bits), c32(a, 32 - bits), "");
    return band(a, lsr(a, x, lo), c32(a, (1u << bits) - 1));
}
static LLVMValueRef sext64(ArmIR* a, LLVMValueRef x) { return LLVMBuildSExt(a->b, x, a->i64, ""); }
/* the signed v (i32 or i64) clamped to lo..hi, and whether it was outside */
static LLVMValueRef clamp(ArmIR* a, LLVMValueRef v, int64_t lo, int64_t hi, LLVMValueRef* saturated) {
    LLVMTypeRef t = LLVMTypeOf(v);
    LLVMValueRef low = LLVMConstInt(t, (unsigned long long)lo, 1), high = LLVMConstInt(t, (unsigned long long)hi, 1);
    LLVMValueRef under = cmp(a, LLVMIntSLT, v, low), over = cmp(a, LLVMIntSGT, v, high);
    *saturated = LLVMBuildOr(a->b, under, over, "");
    return choose(a, under, low, choose(a, over, high, v));
}
/* the i64 v as i32, and whether it didn't fit */
static LLVMValueRef narrow(ArmIR* a, LLVMValueRef v, LLVMValueRef* overflow) {
    LLVMValueRef r = LLVMBuildTrunc(a->b, v, a->i32, "");
    *overflow = cmp(a, LLVMIntNE, sext64(a, r), v);
    return r;
}
/* Q is sticky: an instruction that saturates or overflows sets it, none clears it but MSR */
static void sticky_q(ArmIR* a, LLVMValueRef set) { PUT(a, cpsr, bor(a, GET(a, cpsr), shl(a, z32(a, set), 27))); }
static LLVMValueRef rdhi_rdlo(ArmIR* a, unsigned hi, unsigned lo) {
    return bor(a, z64(a, reg(a, lo)), LLVMBuildShl(a->b, z64(a, reg(a, hi)), c64(a, 32), ""));
}
static void set_rdhi_rdlo(ArmIR* a, unsigned hi, unsigned lo, LLVMValueRef v) {
    setreg(a, lo, LLVMBuildTrunc(a->b, v, a->i32, ""));
    setreg(a, hi, LLVMBuildTrunc(a->b, LLVMBuildLShr(a->b, v, c64(a, 32), ""), a->i32, ""));
}

/* {S,Q,SH,U,UQ,UH}{ADD16,ASX,SAX,SUB16,ADD8,SUB8}, lane by lane in i32, where no sum or difference overflows. The
   modular forms set GE: a lane's sum or difference >= 0, an unsigned sum's carry out */
static void parallel(ArmIR* a) {
    uint32_t w = a->word;
    unsigned form = (w >> 20) & 3, u = (w >> 22) & 1, op = (w >> 5) & 7, bits = op >= 4 ? 8 : 16, lanes = 32 / bits, i;
    LLVMValueRef x = reg(a, (w >> 16) & 15), y = reg(a, w & 15), r = c32(a, 0), ge = c32(a, 0), unused;
    for (i = 0; i < lanes; ++i) {
        /* ASX: the low lane subtracts Rm's high halfword, the high lane adds its low one. SAX the other way */
        unsigned from = op == 1 || op == 2 ? lanes - 1 - i : i;
        int subtract = op == 3 || op == 7 || (op == 1 && i == 0) || (op == 2 && i == 1);
        LLVMValueRef p = lane(a, x, i * bits, bits, !u), q = lane(a, y, from * bits, bits, !u), v;
        v = subtract ? sub(a, p, q) : add(a, p, q);
        if (form == 1) {
            LLVMValueRef set = u && !subtract ? cmp(a, LLVMIntUGE, v, c32(a, 1u << bits))
                                              : cmp(a, LLVMIntSGE, v, c32(a, 0));
            ge = bor(a, ge, choose(a, set, c32(a, bits == 16 ? 3u << (2 * i) : 1u << i), c32(a, 0)));
        } else if (form == 2)
            v = u ? clamp(a, v, 0, (1 << bits) - 1, &unused)
                  : clamp(a, v, -(1 << (bits - 1)), (1 << (bits - 1)) - 1, &unused);
        else
            v = LLVMBuildAShr(a->b, v, c32(a, 1), "");
        r = bor(a, r, shl(a, band(a, v, c32(a, (1u << bits) - 1)), i * bits));
    }
    if (form == 1) PUT(a, cpsr, bor(a, band(a, GET(a, cpsr), c32(a, ~0x000f0000u)), shl(a, ge, 16)));
    setreg(a, (w >> 12) & 15, r);
}
static void media(ArmIR* a, SnxA32Kind kind) {
    uint32_t w = a->word;
    unsigned n = (w >> 16) & 15, d = (w >> 12) & 15, s = (w >> 8) & 15, op, i;
    LLVMValueRef x = reg(a, w & 15), r, v, saturated, unused;
    switch (kind) {
        case SNX_A32_SATURATE: {
            int u = !!(w & (1u << 22));
            int64_t lo, hi;
            if ((w & 0x0fb00ff0) == 0x06a00f30) { /* SSAT16, USAT16: to 1..16 or 0..15 bits */
                unsigned bits = ((w >> 16) & 15) + !u;
                LLVMValueRef any = LLVMConstInt(a->i1, 0, 0);
                lo = u ? 0 : -((int64_t)1 << (bits - 1));
                hi = u ? ((int64_t)1 << bits) - 1 : ((int64_t)1 << (bits - 1)) - 1;
                r = c32(a, 0);
                for (i = 0; i < 2; ++i) {
                    v = clamp(a, lane(a, x, i * 16, 16, 1), lo, hi, &saturated);
                    any = LLVMBuildOr(a->b, any, saturated, "");
                    r = bor(a, r, shl(a, band(a, v, c32(a, 0xffff)), i * 16));
                }
                saturated = any;
            } else { /* SSAT, USAT: Rm LSL or ASR (ASR #32 encoded as 0), to 1..32 or 0..31 bits */
                unsigned bits = ((w >> 16) & 31) + !u;
                lo = u ? 0 : -((int64_t)1 << (bits - 1));
                hi = u ? ((int64_t)1 << bits) - 1 : ((int64_t)1 << (bits - 1)) - 1;
                v = shift(a, x, (w >> 5) & 2, NULL, 1, (w >> 7) & 31, &unused);
                r = LLVMBuildTrunc(a->b, clamp(a, sext64(a, v), lo, hi, &saturated), a->i32, "");
            }
            sticky_q(a, saturated);
            setreg(a, d, r);
            return;
        }
        case SNX_A32_PACK: /* PKHBT: Rn's bottom, Rm LSL's top. PKHTB: Rn's top, Rm ASR's bottom */
            v = shift(a, x, w & 0x40 ? 2 : 0, NULL, 1, (w >> 7) & 31, &unused);
            r = w & 0x40 ? bor(a, band(a, reg(a, n), c32(a, 0xffff0000u)), band(a, v, c32(a, 0xffff)))
                         : bor(a, band(a, reg(a, n), c32(a, 0xffff)), band(a, v, c32(a, 0xffff0000u)));
            setreg(a, d, r);
            return;
        case SNX_A32_SELECT: { /* each byte from Rn where its GE bit is set, else from Rm */
            LLVMValueRef ge = lsr(a, GET(a, cpsr), 16), mask = c32(a, 0);
            for (i = 0; i < 4; ++i)
                mask = bor(a, mask,
                           choose(a, cmp(a, LLVMIntNE, band(a, ge, c32(a, 1u << i)), c32(a, 0)), c32(a, 0xffu << (8 * i)),
                                  c32(a, 0)));
            setreg(a, d, bor(a, band(a, reg(a, n), mask), band(a, x, LLVMBuildNot(a->b, mask, ""))));
            return;
        }
        case SNX_A32_QARITH: { /* Rm + or - Rn, Rn doubled with saturation first for QDADD and QDSUB */
            LLVMValueRef y = sext64(a, reg(a, n)), first = LLVMConstInt(a->i1, 0, 0);
            op = (w >> 21) & 3;
            if (op & 2) y = clamp(a, add(a, y, y), INT32_MIN, INT32_MAX, &first);
            v = op & 1 ? sub(a, sext64(a, x), y) : add(a, sext64(a, x), y);
            r = LLVMBuildTrunc(a->b, clamp(a, v, INT32_MIN, INT32_MAX, &saturated), a->i32, "");
            sticky_q(a, LLVMBuildOr(a->b, first, saturated, ""));
            setreg(a, d, r);
            return;
        }
        case SNX_A32_HALF_MUL: { /* Rd (RdHi) n, Ra (RdLo) d, Rm s, Rn the low field: N picks Rn's half, M Rm's */
            LLVMValueRef m = lane(a, reg(a, s), w & 0x40 ? 16 : 0, 16, 1);
            op = (w >> 21) & 3;
            if (op == 1) { /* SMLAW<y>, SMULW<y>: the top 32 bits of the 48-bit product */
                v = LLVMBuildAShr(a->b, LLVMBuildMul(a->b, sext64(a, x), sext64(a, m), ""), c64(a, 16), "");
                if (w & 0x20) {
                    setreg(a, n, LLVMBuildTrunc(a->b, v, a->i32, ""));
                    return;
                }
                r = narrow(a, add(a, sext64(a, LLVMBuildTrunc(a->b, v, a->i32, "")), sext64(a, reg(a, d))), &saturated);
                sticky_q(a, saturated);
                setreg(a, n, r);
                return;
            }
            v = LLVMBuildMul(a->b, lane(a, x, w & 0x20 ? 16 : 0, 16, 1), m, ""); /* at most 2^30 */
            if (op == 0) {                                                       /* SMLA<x><y>: Q on overflow */
                r = narrow(a, add(a, sext64(a, v), sext64(a, reg(a, d))), &saturated);
                sticky_q(a, saturated);
                setreg(a, n, r);
            } else if (op == 2) /* SMLAL<x><y> */
                set_rdhi_rdlo(a, n, d, add(a, rdhi_rdlo(a, n, d), sext64(a, v)));
            else
                setreg(a, n, v);
            return;
        }
        case SNX_A32_DUAL_MUL: { /* Rd (RdHi) n, Ra (RdLo) d, Rm s, Rn the low field */
            LLVMValueRef m = reg(a, s);
            op = (w >> 5) & 7;
            if (((w >> 20) & 7) == 5) { /* SMMLA, SMMUL, SMMLS: the top 32 bits of (Ra << 32) +/- Rn * Rm, R rounds */
                LLVMValueRef acc = d != 15 ? LLVMBuildShl(a->b, z64(a, reg(a, d)), c64(a, 32), "") : c64(a, 0);
                v = LLVMBuildMul(a->b, sext64(a, x), sext64(a, m), "");
                v = op >= 6 ? sub(a, acc, v) : add(a, acc, v);
                if (w & 0x20) v = add(a, v, c64(a, 0x80000000u));
                setreg(a, n, LLVMBuildTrunc(a->b, LLVMBuildLShr(a->b, v, c64(a, 32), ""), a->i32, ""));
                return;
            }
            if (w & 0x20) m = ror(a, m, 16); /* X: exchange Rm's halves */
            r = LLVMBuildMul(a->b, lane(a, x, 0, 16, 1), lane(a, m, 0, 16, 1), "");
            v = LLVMBuildMul(a->b, lane(a, x, 16, 16, 1), lane(a, m, 16, 16, 1), "");
            v = op & 2 ? sub(a, sext64(a, r), sext64(a, v)) : add(a, sext64(a, r), sext64(a, v));
            if (((w >> 20) & 7) == 4) { /* SMLALD, SMLSLD */
                set_rdhi_rdlo(a, n, d, add(a, rdhi_rdlo(a, n, d), v));
                return;
            }
            if (d != 15) v = add(a, v, sext64(a, reg(a, d))); /* SMLAD, SMLSD; SMUAD and SMUSD without Ra */
            r = narrow(a, v, &saturated);
            sticky_q(a, saturated);
            setreg(a, n, r);
            return;
        }
        case SNX_A32_ABS_DIFF: { /* USAD8, USADA8: Rd n, Ra d, Rm s, Rn the low field */
            LLVMValueRef m = reg(a, s);
            r = d != 15 ? reg(a, d) : c32(a, 0);
            for (i = 0; i < 4; ++i) {
                v = sub(a, lane(a, x, i * 8, 8, 0), lane(a, m, i * 8, 8, 0));
                r = add(a, r, choose(a, cmp(a, LLVMIntSLT, v, c32(a, 0)), sub(a, c32(a, 0), v), v));
            }
            setreg(a, n, r);
            return;
        }
        case SNX_A32_CRC32: { /* the reflected CRC of Rm's low 1, 2 or 4 bytes onto Rn, without inversions */
            unsigned bits = 8u << ((w >> 21) & 3);
            LLVMValueRef poly = c32(a, w & 0x200 ? 0x82f63b78u : 0xedb88320u);
            r = bxor(a, reg(a, n), bits == 32 ? x : band(a, x, c32(a, (1u << bits) - 1)));
            for (i = 0; i < bits; ++i)
                r = bxor(a, lsr(a, r, 1), band(a, poly, sub(a, c32(a, 0), band(a, r, c32(a, 1)))));
            setreg(a, d, r);
            return;
        }
        default: /* SETEND */
            PUT(a, cpsr, bor(a, band(a, GET(a, cpsr), c32(a, ~SNX_ARM_E)), c32(a, w & SNX_ARM_E)));
            return;
    }
}
static void miscellaneous(ArmIR* a, SnxA32Kind kind) {
    uint32_t w = a->word;
    unsigned d = (w >> 12) & 15, n = (w >> 16) & 15;
    LLVMValueRef x = reg(a, w & 15), r = x;
    if (kind == SNX_A32_MOVW || kind == SNX_A32_MOVT) {
        uint32_t imm = ((w >> 4) & 0xf000) | (w & 0xfff);
        r = kind == SNX_A32_MOVW ? c32(a, imm) : bor(a, band(a, reg(a, d), c32(a, 65535)), c32(a, imm << 16));
    } else if (kind == SNX_A32_DIV) {
        LLVMValueRef y = reg(a, (w >> 8) & 15), zero = cmp(a, LLVMIntEQ, y, c32(a, 0));
        y = choose(a, zero, c32(a, 1), y);
        if (w & (1u << 21))
            r = LLVMBuildUDiv(a->b, x, y, "");
        else {
            r = LLVMBuildSDiv(a->b, LLVMBuildSExt(a->b, x, a->i64, ""), LLVMBuildSExt(a->b, y, a->i64, ""), "");
            r = LLVMBuildTrunc(a->b, r, a->i32, "");
        }
        r = choose(a, zero, c32(a, 0), r);
        d = n;
    } else if (kind == SNX_A32_CLZ) {
        LLVMValueRef args[] = {x, LLVMConstInt(a->i1, 0, 0)};
        r = intrinsic(a, "llvm.ctlz", 9, a->i32, args, 2);
    } else if (kind == SNX_A32_REV) {
        if ((w & 0x00400080) == 0x00400000)
            r = intrinsic(a, "llvm.bitreverse", 15, a->i32, &x, 1);
        else if (!(w & 0x80))
            r = intrinsic(a, "llvm.bswap", 10, a->i32, &x, 1);
        else {
            r = bor(a, band(a, lsr(a, x, 8), c32(a, 0x00ff00ff)), shl(a, band(a, x, c32(a, 0x00ff00ff)), 8));
            if (w & (1u << 22)) r = LLVMBuildSExt(a->b, LLVMBuildTrunc(a->b, r, a->i16, ""), a->i32, "");
        }
    } else if (kind == SNX_A32_EXTEND && !(w & (3u << 20))) { /* SXTB16, UXTB16, with add: bytes 0 and 2 */
        int sign = !(w & (1u << 22));
        LLVMValueRef v = ror(a, x, ((w >> 10) & 3) * 8), lo = lane(a, v, 0, 8, sign), hi = lane(a, v, 16, 8, sign);
        if (n != 15) {
            lo = add(a, lo, reg(a, n));
            hi = add(a, hi, lsr(a, reg(a, n), 16));
        }
        r = bor(a, band(a, lo, c32(a, 0xffff)), shl(a, hi, 16));
    } else if (kind == SNX_A32_EXTEND) {
        LLVMTypeRef type = w & (1u << 20) ? a->i16 : a->i8;
        r = LLVMBuildTrunc(a->b, ror(a, x, ((w >> 10) & 3) * 8), type, "");
        r = w & (1u << 22) ? z32(a, r) : LLVMBuildSExt(a->b, r, a->i32, "");
        if (n != 15) r = add(a, r, reg(a, n));
    } else if (kind == SNX_A32_UADD8 || kind == SNX_A32_SEL) {
        LLVMValueRef y = reg(a, n), ge = c32(a, 0);
        r = c32(a, 0);
        for (unsigned i = 0; i < 4; ++i) {
            LLVMValueRef left = band(a, lsr(a, y, i * 8), c32(a, 255));
            LLVMValueRef right = band(a, lsr(a, x, i * 8), c32(a, 255)), lane;
            if (kind == SNX_A32_UADD8) {
                LLVMValueRef sum = add(a, left, right);
                lane = band(a, sum, c32(a, 255));
                ge = bor(a, ge, shl(a, lsr(a, sum, 8), 16 + i));
            } else
                lane = choose(a, cmp(a, LLVMIntNE, flag(a, 16 + i), c32(a, 0)), left, right);
            r = bor(a, r, shl(a, lane, i * 8));
        }
        if (kind == SNX_A32_UADD8) PUT(a, cpsr, bor(a, band(a, GET(a, cpsr), c32(a, ~0x000f0000u)), ge));
    } else if (kind == SNX_A32_BITFIELD) {
        unsigned lsb = (w >> 7) & 31, width;
        uint32_t mask;
        if ((w & 0x0fe00070) == 0x07c00010) {
            width = ((w >> 16) & 31) - lsb + 1;
            mask = UINT32_MAX >> (32 - width);
            r = (w & 15) == 15 ? c32(a, 0) : shl(a, band(a, x, c32(a, mask)), lsb);
            r = bor(a, band(a, reg(a, d), c32(a, ~(mask << lsb))), r);
        } else {
            width = ((w >> 16) & 31) + 1;
            mask = UINT32_MAX >> (32 - width);
            r = band(a, lsr(a, x, lsb), c32(a, mask));
            if (!(w & (1u << 22)) && width < 32) r = LLVMBuildAShr(a->b, shl(a, r, 32 - width), c32(a, 32 - width), "");
        }
    } else if (kind == SNX_A32_MRS) /* the execution state bits (IT, J, T) read as zero */
        r = band(a, GET(a, cpsr), c32(a, ~(0x0600FC00u | (1u << 24) | SNX_ARM_T)));
    else if (kind == SNX_A32_MSR) {
        LLVMValueRef unused;
        uint32_t mask = (n & 8 ? 0xf8000000u : 0) | (n & 4 ? 0x000f0000u : 0) | (n & 2 ? SNX_ARM_E : 0);
        r = w & (1u << 25) ? operand2(a, w, &unused) : x;
        PUT(a, cpsr, bor(a, band(a, GET(a, cpsr), c32(a, ~mask)), band(a, r, c32(a, mask))));
        return;
    }
    setreg(a, d, r);
}
static int instruction(ArmIR* a, SnxA32Kind kind); /* Thumb32 runs A32 twins through it (TH_A32) */
#include "common/cpu/arm/llvm_vector.inc"
#include "common/cpu/arm/llvm_thumb.inc"

/* Returns 1 for instructions which end the block, with the next PC stored. */
static int instruction(ArmIR* a, SnxA32Kind kind) {
    uint32_t w = a->word;
    unsigned d = (w >> 12) & 15;
    if (a->memory == SNX_ARM_MEMORY_CHECKED &&
        ((kind == SNX_A32_VECTOR && a->profile != SNX_ARM_PROFILE_ARM11) || kind == SNX_A32_ACQUIRE_RELEASE ||
         (kind == SNX_A32_EXCLUSIVE && a->profile != SNX_ARM_PROFILE_ARM11))) {
        trap(a, SNX_ARM_BACKEND_UNSUPPORTED, c32(a, w));
        return 1;
    }
    switch (kind) {
        case SNX_A32_VECTOR: vector_instruction(a); break;
        case SNX_A32_DATA: data(a); return d == 15;
        case SNX_A32_SINGLE:
        case SNX_A32_EXTRA: memory(a, kind); return d == 15 && (w & (1u << 20));
        case SNX_A32_MULTIPLE: multiple(a); return (w & (1u << 15)) && (w & (1u << 20));
        case SNX_A32_BRANCH:
        case SNX_A32_BLX_IMM: {
            uint32_t offset = (w & 0xffffff) << 2;
            if (offset & 0x02000000) offset |= 0xfc000000;
            if ((w & (1u << 24)) || kind == SNX_A32_BLX_IMM) setreg(a, 14, c32(a, a->pc + 4));
            if (kind == SNX_A32_BLX_IMM) {
                offset |= (w >> 23) & 2;
                PUT(a, cpsr, bor(a, GET(a, cpsr), c32(a, SNX_ARM_T)));
            }
            setreg(a, 15, c32(a, a->pc + 8 + offset));
            return 1;
        }
        case SNX_A32_BX: {
            LLVMValueRef target = reg(a, w & 15);
            /* Target can be LR, so read it before writing the return address. */
            branch_exchange(a, target);
            if (w & 0x20) setreg(a, 14, c32(a, a->pc + 4));
            return 1;
        }
        case SNX_A32_MUL:
        case SNX_A32_MULL:
        case SNX_A32_MLS: multiply(a, kind); break;
        case SNX_A32_MULH: halfword_multiply(a); break;
        case SNX_A32_EXCLUSIVE:
            if (a->memory == SNX_ARM_MEMORY_CHECKED)
                checked_exclusive(a);
            else
                exclusive(a);
            break;
        case SNX_A32_ACQUIRE_RELEASE: acquire_release(a); break;
        case SNX_A32_CLREX: PUT(a, exclusive_valid, c32(a, 0)); break;
        case SNX_A32_BARRIER:
            /* Stronger than each valid DMB/DSB scope, never just a compiler barrier. */
            LLVMBuildFence(a->b, LLVMAtomicOrderingSequentiallyConsistent, 0, "");
            break;
        case SNX_A32_HINT: break; /* hints may complete without waiting */
        case SNX_A32_PARALLEL: parallel(a); break;
        case SNX_A32_SATURATE:
        case SNX_A32_PACK:
        case SNX_A32_SELECT:
        case SNX_A32_QARITH:
        case SNX_A32_HALF_MUL:
        case SNX_A32_DUAL_MUL:
        case SNX_A32_ABS_DIFF:
        case SNX_A32_CRC32: media(a, kind); break;
        case SNX_A32_SETEND:
        case SNX_A32_MSR:
            /* A block checks E only on entry: one that may have changed it ends here */
            if (kind == SNX_A32_SETEND)
                media(a, kind);
            else
                miscellaneous(a, kind);
            if (kind == SNX_A32_MSR && !(w & (1u << 17))) break;
            setreg(a, 15, c32(a, a->next));
            return 1;
        case SNX_A32_COPROC: {    /* the backend's (SnxArmRt.coproc): it reads or writes the registers, or stops here */
            LLVMValueRef word = c32(a, w);
            rt_call(a, "snx_arm_coproc", &word, 1);
            stop_on_exit(a);
            break;
        }
        case SNX_A32_SVC:
        case SNX_A32_BKPT:
            setreg(a, 15, c32(a, a->pc + 4));
            trap(a, kind == SNX_A32_SVC ? SNX_ARM_SVC : SNX_ARM_BREAKPOINT,
                 c32(a, kind == SNX_A32_SVC ? w & 0xffffff : ((w >> 4) & 0xfff0) | (w & 15)));
            return 1;
        case SNX_A32_UNDEFINED: trap(a, SNX_ARM_UNDEFINED, c32(a, w)); return 1;
        default: miscellaneous(a, kind); break;
    }
    return 0;
}

LLVMValueRef snx_arm_llvm_block(LLVMModuleRef module, const char* name, const uint32_t* words, size_t count,
                                uint32_t pc, size_t* consumed) {
    return snx_arm_llvm_block_ex(module, name, words, count, pc, consumed, NULL);
}
LLVMValueRef snx_arm_llvm_block_ex(LLVMModuleRef module, const char* name, const uint32_t* words, size_t count,
                                   uint32_t pc, size_t* consumed, const SnxArmCodegenOptions* options) {
    ArmIR a = {0};
    LLVMTypeRef ft;
    size_t i;
    if (consumed) *consumed = 0;
    if (!options_valid(options) || !module || !name || !*name || !words || !count || count > 65536 || (pc & 3) ||
        LLVMGetNamedFunction(module, name) || LLVMGetNamedGlobal(module, name))
        return NULL;
    if (options) {
        a.profile = options->profile;
        a.memory = options->memory;
    }
    a.ctx = LLVMGetModuleContext(module);
    a.mod = module;
    a.b = LLVMCreateBuilderInContext(a.ctx);
    a.i1 = LLVMInt1TypeInContext(a.ctx);
    a.i8 = LLVMInt8TypeInContext(a.ctx);
    a.i16 = LLVMInt16TypeInContext(a.ctx);
    a.i32 = LLVMInt32TypeInContext(a.ctx);
    a.i64 = LLVMInt64TypeInContext(a.ctx);
    a.ptr = LLVMPointerTypeInContext(a.ctx, 0);
    ft = LLVMFunctionType(LLVMVoidTypeInContext(a.ctx), &a.ptr, 1, 0);
    a.fn = LLVMAddFunction(module, name, ft);
    a.cpu = LLVMGetParam(a.fn, 0);
    LLVMPositionBuilderAtEnd(a.b, block(&a, "entry"));
    PUT(&a, exit, c32(&a, SNX_ARM_OK));
    PUT(&a, syndrome, c32(&a, 0));
    require(&a, cmp(&a, LLVMIntEQ, get(&a, offsetof(SnxArmCPU, r) + 60, a.i32), c32(&a, pc)), SNX_ARM_BAD_ENTRY,
            c32(&a, pc));
    require(&a, cmp(&a, LLVMIntEQ, band(&a, GET(&a, cpsr), c32(&a, SNX_ARM_T)), c32(&a, 0)), SNX_ARM_BAD_ENTRY,
            c32(&a, words[0]));
    require(&a, cmp(&a, LLVMIntEQ, band(&a, GET(&a, cpsr), c32(&a, SNX_ARM_E)), c32(&a, 0)), SNX_ARM_ENDIAN_UNSUPPORTED,
            c32(&a, words[0]));
    memory_entry(&a);
    for (i = 0; i < count; ++i) {
        LLVMBasicBlockRef execute = block(&a, "execute"), skip = block(&a, "skip"), next = block(&a, "next");
        int stop;
        a.pc = pc + (uint32_t)i * 4;
        a.next = a.pc + 4;
        a.word = words[i];
        setreg(&a, 15, c32(&a, a.pc));
        instruction_fetch(&a, a.word, 4);
        LLVMBuildCondBr(a.b, condition(&a, a.word >> 28), execute, skip);
        LLVMPositionBuilderAtEnd(a.b, execute);
        stop = instruction(&a, snx_a32_decode_profile(a.profile, a.word));
        if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(a.b))) {
            if (!stop) setreg(&a, 15, c32(&a, a.pc + 4));
            LLVMBuildBr(a.b, next);
        }
        LLVMPositionBuilderAtEnd(a.b, skip);
        setreg(&a, 15, c32(&a, a.pc + 4));
        LLVMBuildBr(a.b, next);
        LLVMPositionBuilderAtEnd(a.b, next);
        if (stop) {
            ++i;
            break;
        }
    }
    LLVMBuildRetVoid(a.b);
    LLVMDisposeBuilder(a.b);
    if (consumed) *consumed = i;
    return a.fn;
}
