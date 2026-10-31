/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/cpu/recompiler/llvm/ir.h"
#include "common/cpu/aarch64/cpu.h"

/* A register snapshot is only valid in its owning basic block. Guest state is
   still written at every architectural store; this cache forwards IR values. */
void ir_registers_invalidate(SnxIr* ir) {
    ir->state.registers.valid = 0;
    ir->state.registers.block = NULL;
}

void ir_vectors_invalidate(SnxIr* ir) {
    for (unsigned i = 0; i < 4; i++) ir->state.vectors.valid[i] = 0;
    ir->state.vectors.block = NULL;
}

/* Unknown calls and aliases invalidate every cached architectural domain. */
void ir_state_invalidate(SnxIr* ir) {
    ir_registers_invalidate(ir);
    ir_vectors_invalidate(ir);
    ir_flags_invalidate(ir);
}

static void registers_block(SnxIr* ir) {
    LLVMBasicBlockRef block = LLVMGetInsertBlock(ir->b);
    if (ir->state.registers.block == block) return;
    ir->state.registers.valid = 0;
    ir->state.registers.block = block;
}

/* Flags are N, Z, C, V in their architectural order. Avoid carrying
   a value across block boundaries: PHI inputs and unknown calls may change it. */
static const size_t flag_offsets[4] = {offsetof(CPU, n), offsetof(CPU, z), offsetof(CPU, cf), offsetof(CPU, vf)};

void ir_flags_invalidate(SnxIr* ir) {
    ir->state.flags.valid = 0;
    ir->state.flags.block = NULL;
    ir->state.flags.origin = SNX_IR_FLAGS_SOME;
}

static SnxIrFlags* flags_block(SnxIr* ir) {
    SnxIrFlags* flags = &ir->state.flags;
    LLVMBasicBlockRef block = LLVMGetInsertBlock(ir->b);
    if (flags->block != block) {
        flags->valid = 0;
        flags->block = block;
        flags->origin = SNX_IR_FLAGS_SOME;
    }
    return flags;
}

LLVMValueRef ir_flag_read(SnxIr* ir, unsigned index) {
    SnxIrFlags* flags = flags_block(ir);
    unsigned bit = 1u << index;
    if (!(flags->valid & bit)) {
        flags->value[index] = ir_cpu_load(ir, ir->t_i32, flag_offsets[index]);
        flags->valid |= bit;
    }
    return flags->value[index];
}

static void flags_commit(SnxIr* ir, LLVMValueRef n, LLVMValueRef z, LLVMValueRef c, LLVMValueRef v) {
    SnxIrFlags* flags = flags_block(ir);
    LLVMValueRef values[4] = {n, z, c, v};
    for (unsigned i = 0; i < 4; ++i) {
        LLVMValueRef bits = ir_u32(ir, values[i]);
        ir_tag_cpu(ir, LLVMBuildStore(ir->b, bits, ir_cpu(ir, flag_offsets[i])));
        flags->value[i] = bits;
    }
    flags->valid = 15;
    flags->origin = SNX_IR_FLAGS_SOME;
}

static void flags_origin(SnxIr* ir, unsigned origin, LLVMValueRef left, LLVMValueRef right) {
    ir->state.flags.origin = origin;
    ir->state.flags.left = left;
    ir->state.flags.right = right;
}

LLVMValueRef ir_flags_adds(SnxIr* ir, unsigned bits, LLVMValueRef a, LLVMValueRef b, LLVMValueRef cin) {
    LLVMTypeRef type = bits == 64 ? ir->t_i64 : ir->t_i32;
    LLVMValueRef r = LLVMBuildAdd(ir->b, LLVMBuildAdd(ir->b, a, b, ""), ir_cast(ir, cin, type, 0), "");
    LLVMValueRef sign = LLVMConstInt(type, bits - 1, 0);
    LLVMValueRef n = LLVMBuildLShr(ir->b, r, sign, "");
    LLVMValueRef z = ir_bool32(ir, LLVMBuildICmp(ir->b, LLVMIntEQ, r, LLVMConstNull(type), ""));
    LLVMValueRef cf =
        LLVMBuildSelect(ir->b, LLVMBuildICmp(ir->b, LLVMIntNE, cin, ir_i32(ir, 0), ""),
                        LLVMBuildICmp(ir->b, LLVMIntULE, r, a, ""), LLVMBuildICmp(ir->b, LLVMIntULT, r, a, ""), "");
    LLVMValueRef overflow =
        LLVMBuildAnd(ir->b, LLVMBuildNot(ir->b, LLVMBuildXor(ir->b, a, b, ""), ""), LLVMBuildXor(ir->b, a, r, ""), "");
    flags_commit(ir, n, z, ir_bool32(ir, cf), LLVMBuildLShr(ir->b, overflow, sign, ""));
    if (LLVMIsAConstantInt(cin) && !LLVMConstIntGetZExtValue(cin)) flags_origin(ir, SNX_IR_FLAGS_ADD, a, b);
    return r;
}

LLVMValueRef ir_flags_subs(SnxIr* ir, unsigned bits, LLVMValueRef a, LLVMValueRef b) {
    LLVMTypeRef type = bits == 64 ? ir->t_i64 : ir->t_i32;
    LLVMValueRef r = LLVMBuildSub(ir->b, a, b, "");
    LLVMValueRef sign = LLVMConstInt(type, bits - 1, 0);
    LLVMValueRef overflow = LLVMBuildAnd(ir->b, LLVMBuildXor(ir->b, a, b, ""), LLVMBuildXor(ir->b, a, r, ""), "");
    flags_commit(ir, LLVMBuildLShr(ir->b, r, sign, ""),
                 ir_bool32(ir, LLVMBuildICmp(ir->b, LLVMIntEQ, r, LLVMConstNull(type), "")),
                 ir_bool32(ir, LLVMBuildICmp(ir->b, LLVMIntUGE, a, b, "")), LLVMBuildLShr(ir->b, overflow, sign, ""));
    flags_origin(ir, SNX_IR_FLAGS_SUB, a, b);
    return r;
}

void ir_flags_logic(SnxIr* ir, LLVMValueRef value, unsigned bits) {
    flags_commit(ir, LLVMBuildLShr(ir->b, value, LLVMConstInt(LLVMTypeOf(value), bits - 1, 0), ""),
                 ir_bool32(ir, LLVMBuildICmp(ir->b, LLVMIntEQ, value, LLVMConstNull(LLVMTypeOf(value)), "")),
                 ir_i32(ir, 0), ir_i32(ir, 0));
    flags_origin(ir, SNX_IR_FLAGS_LOGIC, value, NULL);
}

void ir_flags_set_nzcv(SnxIr* ir, LLVMValueRef nzcv) {
    LLVMValueRef one = ir_i32(ir, 1);
    LLVMValueRef n = LLVMBuildAnd(ir->b, LLVMBuildLShr(ir->b, nzcv, ir_i32(ir, 3), ""), one, "");
    LLVMValueRef z = LLVMBuildAnd(ir->b, LLVMBuildLShr(ir->b, nzcv, ir_i32(ir, 2), ""), one, "");
    LLVMValueRef c = LLVMBuildAnd(ir->b, LLVMBuildLShr(ir->b, nzcv, one, ""), one, "");
    flags_commit(ir, n, z, c, LLVMBuildAnd(ir->b, nzcv, one, ""));
}

/* Condition code (cond >> 1, but AL) on flags a compare gave, as an i1 of that compare: NULL for VS of SUB and ADD
   and the ones of ADD a variable needs the overflow for. CMN with a constant k other than 0 and the most negative
   value gives the four flags of CMP with -k (its carry is a + k >= 2^n, so a >= -k; its overflow that of the same
   sum), and ANDS those of CMP of its result with 0, but for C, which ANDS clears */
static LLVMValueRef origin_cond(SnxIr* ir, const SnxIrFlags* flags, unsigned code) {
    LLVMValueRef left = flags->left, right = flags->right;
    LLVMTypeRef type = LLVMTypeOf(left);
    unsigned origin = flags->origin, bits = LLVMGetIntTypeWidth(type);
    static const LLVMIntPredicate sub[7] = {LLVMIntEQ, LLVMIntUGE, LLVMIntSLT, 0, LLVMIntUGT, LLVMIntSGE, LLVMIntSGT};
    LLVMValueRef zero = LLVMConstNull(type);
    if (origin == SNX_IR_FLAGS_ADD && LLVMIsAConstantInt(right)) {
        SnxU64 k = LLVMConstIntGetZExtValue(right), top = (SnxU64)1 << (bits - 1);
        if (k != 0 && k != top) {
            origin = SNX_IR_FLAGS_SUB;
            right = LLVMConstNeg(right);
        }
    }
    switch (origin) {
        case SNX_IR_FLAGS_SUB:
            if (code == 3) return NULL;
            if (code == 2) return LLVMBuildICmp(ir->b, LLVMIntSLT, LLVMBuildSub(ir->b, left, right, ""), zero, "");
            return LLVMBuildICmp(ir->b, sub[code], left, right, "");
        case SNX_IR_FLAGS_ADD: {
            LLVMValueRef r = LLVMBuildAdd(ir->b, left, right, "");
            if (code == 0) return LLVMBuildICmp(ir->b, LLVMIntEQ, r, zero, "");
            if (code == 1) return LLVMBuildICmp(ir->b, LLVMIntULT, r, left, ""); /* the carry out */
            if (code == 2) return LLVMBuildICmp(ir->b, LLVMIntSLT, r, zero, "");
            return NULL;
        }
        case SNX_IR_FLAGS_LOGIC:
            if (code == 1 || code == 3 || code == 4) return LLVMConstInt(ir->t_i1, 0, 0); /* C and V are 0 */
            if (code == 2) return LLVMBuildICmp(ir->b, LLVMIntSLT, left, zero, "");
            return LLVMBuildICmp(ir->b, sub[code], left, zero, "");
        default: return NULL;
    }
}

LLVMValueRef ir_flags_cond(SnxIr* ir, unsigned cond) {
    unsigned code = cond >> 1;
    if (code == 7) return ir_i32(ir, 1);
    LLVMValueRef r, z, c, n, v;
    SnxIrFlags* flags = flags_block(ir);
    if (flags->origin != SNX_IR_FLAGS_SOME && (r = origin_cond(ir, flags, code)) != NULL) {
        if (cond & 1) r = LLVMBuildNot(ir->b, r, "");
        return ir_bool32(ir, r);
    }
    switch (code) {
        case 0: r = LLVMBuildICmp(ir->b, LLVMIntNE, ir_flag_read(ir, 1), ir_i32(ir, 0), ""); break;
        case 1: r = LLVMBuildICmp(ir->b, LLVMIntNE, ir_flag_read(ir, 2), ir_i32(ir, 0), ""); break;
        case 2: r = LLVMBuildICmp(ir->b, LLVMIntNE, ir_flag_read(ir, 0), ir_i32(ir, 0), ""); break;
        case 3: r = LLVMBuildICmp(ir->b, LLVMIntNE, ir_flag_read(ir, 3), ir_i32(ir, 0), ""); break;
        case 4:
            c = ir_flag_read(ir, 2);
            z = ir_flag_read(ir, 1);
            r = LLVMBuildAnd(ir->b, LLVMBuildICmp(ir->b, LLVMIntNE, c, ir_i32(ir, 0), ""),
                             LLVMBuildICmp(ir->b, LLVMIntEQ, z, ir_i32(ir, 0), ""), "");
            break;
        case 5:
        case 6:
            n = ir_flag_read(ir, 0);
            v = ir_flag_read(ir, 3);
            r = LLVMBuildICmp(ir->b, LLVMIntEQ, n, v, "");
            if (code == 6) {
                z = ir_flag_read(ir, 1);
                r = LLVMBuildAnd(ir->b, r, LLVMBuildICmp(ir->b, LLVMIntEQ, z, ir_i32(ir, 0), ""), "");
            }
            break;
        default: return ir_i32(ir, 1);
    }
    if (cond & 1) r = LLVMBuildNot(ir->b, r, "");
    return ir_bool32(ir, r);
}

/* ---- the CPU ---- */

LLVMValueRef ir_cpu(SnxIr* ir, size_t offset) {
    LLVMValueRef index = ir_i64(ir, offset);
    if (!offset) return ir->c;
    return LLVMBuildInBoundsGEP2(ir->b, ir->t_i8, ir->c, &index, 1, "");
}
LLVMValueRef ir_load(SnxIr* ir, LLVMTypeRef type, LLVMValueRef pointer) {
    return LLVMBuildLoad2(ir->b, type, pointer, "");
}
void ir_store(SnxIr* ir, LLVMValueRef value, LLVMValueRef pointer) {
    /* Arbitrary destinations may alias CPU state. */
    ir_state_invalidate(ir);
    LLVMBuildStore(ir->b, value, pointer);
}
/* The guest's memory is its own allocation, never the CPU structure: telling LLVM lets it keep guest registers in
   host registers across guest loads and stores (and loops), which it can't prove from CPU* noalias alone once c is
   passed to a call */
LLVMValueRef ir_tag_cpu(SnxIr* ir, LLVMValueRef access) {
    LLVMSetMetadata(access, ir->kind_scope, ir->scope_cpu);
    LLVMSetMetadata(access, ir->kind_noalias, ir->scope_guest);
    return access;
}
LLVMValueRef ir_tag_guest(SnxIr* ir, LLVMValueRef access) {
    LLVMSetMetadata(access, ir->kind_scope, ir->scope_guest);
    LLVMSetMetadata(access, ir->kind_noalias, ir->scope_cpu);
    return access;
}
LLVMValueRef ir_cpu_load(SnxIr* ir, LLVMTypeRef type, size_t offset) {
    return ir_tag_cpu(ir, ir_load(ir, type, ir_cpu(ir, offset)));
}
void ir_cpu_store(SnxIr* ir, LLVMValueRef value, size_t offset) {
    for (unsigned i = 0; i < 4; ++i)
        if (offset == flag_offsets[i]) ir_flags_invalidate(ir);
    /* Flag and FP-state stores cannot change the disjoint GPR array. */
    if (offset >= offsetof(CPU, x) && offset < offsetof(CPU, x) + sizeof(((CPU*)0)->x)) ir_registers_invalidate(ir);
    if (offset >= offsetof(CPU, v) && offset < offsetof(CPU, v) + sizeof(((CPU*)0)->v)) ir_vectors_invalidate(ir);
    ir_tag_cpu(ir, LLVMBuildStore(ir->b, value, ir_cpu(ir, offset)));
}

size_t ir_off_x(unsigned n) { return offsetof(CPU, x) + 8 * (size_t)n; }
size_t ir_off_v(unsigned n) { return offsetof(CPU, v) + 16 * (size_t)n; }
size_t ir_off_excl_on(void) { return offsetof(CPU, excl_on); }
size_t ir_off_mem(void) { return offsetof(CPU, mem); }

LLVMValueRef ir_x(SnxIr* ir, unsigned n) {
    registers_block(ir);
    uint32_t bit = (uint32_t)1 << n;
    if (!(ir->state.registers.valid & bit)) {
        ir->state.registers.x[n] = ir_cpu_load(ir, ir->t_i64, ir_off_x(n));
        ir->state.registers.valid |= bit;
    }
    return ir->state.registers.x[n];
}
void ir_set_x(SnxIr* ir, unsigned n, LLVMValueRef value) {
    LLVMValueRef bits = ir_u64(ir, value);
    registers_block(ir);
    ir_tag_cpu(ir, LLVMBuildStore(ir->b, bits, ir_cpu(ir, ir_off_x(n))));
    ir->state.registers.x[n] = bits;
    ir->state.registers.valid |= (uint32_t)1 << n;
}
LLVMValueRef ir_rx(SnxIr* ir, unsigned n) { return n == 31 ? ir_i64(ir, 0) : ir_x(ir, n); }
void ir_setx(SnxIr* ir, unsigned n, LLVMValueRef value) {
    /* the value is computed even for XZR: it may set flags or touch memory */
    if (n != 31) ir_set_x(ir, n, value);
}
LLVMValueRef ir_vd(SnxIr* ir, unsigned n, unsigned lane) {
    return ir_ve(ir, n, 3, lane);
}
void ir_set_vd(SnxIr* ir, unsigned n, unsigned lane, LLVMValueRef value) {
    ir_cpu_store(ir, ir_guest_order(ir, ir_u64(ir, value)), ir_off_v(n) + 8 * (size_t)lane);
}
static LLVMTypeRef lane_type(SnxIr* ir, unsigned size) {
    return size == 0 ? ir->t_i8 : size == 1 ? ir->t_i16 : size == 2 ? ir->t_i32 : ir->t_i64;
}
LLVMValueRef ir_ve(SnxIr* ir, unsigned n, unsigned size, unsigned index) {
    if (index || size >= 4)
        return ir_guest_order(ir, ir_cpu_load(ir, lane_type(ir, size), ir_off_v(n) + ((size_t)index << size)));
    SnxIrVectorCache* cache = &ir->state.vectors;
    LLVMBasicBlockRef block = LLVMGetInsertBlock(ir->b);
    if (cache->block != block) {
        ir_vectors_invalidate(ir);
        cache->block = block;
    }
    uint32_t bit = (uint32_t)1 << n;
    if (!(cache->valid[size] & bit)) {
        cache->low[n][size] = ir_guest_order(ir, ir_cpu_load(ir, lane_type(ir, size), ir_off_v(n)));
        cache->valid[size] |= bit;
    }
    return cache->low[n][size];
}
void ir_set_ve(SnxIr* ir, unsigned n, unsigned size, unsigned index, LLVMValueRef value) {
    ir_cpu_store(ir, ir_guest_order(ir, ir_cast(ir, value, lane_type(ir, size), 0)),
                 ir_off_v(n) + ((size_t)index << size));
}
LLVMValueRef ir_pc(SnxIr* ir) { return ir_cpu_load(ir, ir->t_i64, offsetof(CPU, pc)); }
void ir_set_pc(SnxIr* ir, LLVMValueRef value) { ir_cpu_store(ir, value, offsetof(CPU, pc)); }

/* ---- guest memory ---- */

LLVMValueRef ir_guest_order(SnxIr* ir, LLVMValueRef value) {
    LLVMTypeRef type = LLVMTypeOf(value), lane = type;
    unsigned bits;
    if (!ir->big_endian) return value;
    if (LLVMGetTypeKind(type) == LLVMVectorTypeKind) lane = LLVMGetElementType(type);
    switch (LLVMGetTypeKind(lane)) {
        case LLVMIntegerTypeKind: bits = LLVMGetIntTypeWidth(lane); break;
        case LLVMHalfTypeKind:
        case LLVMBFloatTypeKind: bits = 16; break;
        case LLVMDoubleTypeKind: bits = 64; break;
        default: bits = 32; break;
    }
    if (bits == 8) return value;
    if (LLVMGetTypeKind(lane) == LLVMIntegerTypeKind)
        return ir_intrinsic(ir, "llvm.bswap", type, &value, 1);
    /* a floating lane: swapped as the integer of its width (a lane-for-lane bitcast either way) */
    LLVMTypeRef integer = LLVMIntTypeInContext(ir->ctx, bits);
    if (type != lane) integer = LLVMVectorType(integer, LLVMGetVectorSize(type));
    value = LLVMBuildBitCast(ir->b, value, integer, "");
    value = ir_intrinsic(ir, "llvm.bswap", integer, &value, 1);
    return LLVMBuildBitCast(ir->b, value, type, "");
}

LLVMValueRef ir_mp(SnxIr* ir, LLVMValueRef address) {
    LLVMValueRef index = ir_u64(ir, address);
    return LLVMBuildGEP2(ir->b, ir->t_i8, ir->M, &index, 1, "");
}
/* Guest addresses carry no alignment the host may rely on: the C dereferences of RDn/WRn compile to the same plain
   x86 moves as these align-1 accesses */
/* A guest access is volatile only under the setting volatile_memory (snx_ir_volatile_memory): otherwise LLVM may
   merge two loads of one address, forward a store to a load or drop a store written over, between the barriers and
   atomics, which are fences and seq_cst accesses. The ARMv8 memory model allows all of that: a load may return what
   an earlier one of the same address did, and stores may coalesce. A loop that polls memory another thread writes (a
   relaxed atomic is a plain LDR) keeps its load: every backward branch in a function polls (ir_codegen.c's poll),
   whose compiler barrier no load or store crosses, so LLVM hoists no load out of the loop and sinks no store (the
   call on the poll's cold path alone isn't enough: GVN moves the load onto that path); a loop through other
   functions has their calls */
LLVMValueRef ir_rd(SnxIr* ir, LLVMValueRef address, unsigned bits) {
    LLVMValueRef load = LLVMBuildLoad2(ir->b, ir_int_type(ir, bits), ir_mp(ir, address), "");
    LLVMSetAlignment(load, 1);
    LLVMSetVolatile(load, snx_ir_volatile());
    return ir_guest_order(ir, ir_tag_guest(ir, load));
}
LLVMValueRef ir_rd_bytes(SnxIr* ir, LLVMValueRef address, unsigned bytes) {
    LLVMValueRef load = LLVMBuildLoad2(ir->b, LLVMVectorType(ir->t_i8, bytes), ir_mp(ir, address), "");
    LLVMSetAlignment(load, 1);
    LLVMSetVolatile(load, snx_ir_volatile());
    return ir_tag_guest(ir, load);
}
void ir_wr_bytes(SnxIr* ir, LLVMValueRef address, LLVMValueRef value) {
    LLVMValueRef store = LLVMBuildStore(ir->b, value, ir_mp(ir, address));
    LLVMSetAlignment(store, 1);
    LLVMSetVolatile(store, snx_ir_volatile());
    ir_tag_guest(ir, store);
}
void ir_wr(SnxIr* ir, LLVMValueRef address, unsigned bits, LLVMValueRef value) {
    LLVMValueRef pointer = ir_mp(ir, address);
    LLVMValueRef store =
        LLVMBuildStore(ir->b, ir_guest_order(ir, ir_cast(ir, value, ir_int_type(ir, bits), 0)), pointer);
    LLVMSetAlignment(store, 1);
    LLVMSetVolatile(store, snx_ir_volatile());
    ir_tag_guest(ir, store);
}
/* __atomic_load_n/__atomic_store_n on T* (cpu.h LDACQ/STREL): seq_cst, naturally aligned as the C type says */
LLVMValueRef ir_ldacq(SnxIr* ir, LLVMValueRef address, unsigned bits) {
    LLVMValueRef load = LLVMBuildLoad2(ir->b, ir_int_type(ir, bits), ir_mp(ir, address), "");
    LLVMSetOrdering(load, LLVMAtomicOrderingSequentiallyConsistent);
    LLVMSetAlignment(load, bits / 8);
    return ir_guest_order(ir, ir_tag_guest(ir, load));
}
void ir_strel(SnxIr* ir, LLVMValueRef address, unsigned bits, LLVMValueRef value) {
    LLVMValueRef pointer = ir_mp(ir, address);
    LLVMValueRef store =
        LLVMBuildStore(ir->b, ir_guest_order(ir, ir_cast(ir, value, ir_int_type(ir, bits), 0)), pointer);
    LLVMSetOrdering(store, LLVMAtomicOrderingSequentiallyConsistent);
    LLVMSetAlignment(store, bits / 8);
    ir_tag_guest(ir, store);
}
