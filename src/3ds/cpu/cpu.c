/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "3ds/cpu/ctr_cpu.h"
#include "3ds/memory/host.h"
#include "common/cpu/arm/simd.h"
#include <string.h>

/* VFPv2 short vectors on the ARM11. The block emitter stops a VFP data-processing word under a non-zero LEN or STRIDE
   unless it is a compare or a conversion, which is scalar. This backend runs the operations that ctr_vfp_decode admits
   one element at a time. A word the profile rejects reaches it as SNX_ARM_UNDEFINED and keeps that exit. A lane is read
   by its number (S(2n) = Dn[31:0]), so the host's byte order does not matter. */
#define FPSCR_LEN_SHIFT    16
#define FPSCR_STRIDE_SHIFT 20
#define BANK_BYTES         32u /* a bank of the register file: 8 singles or 4 doubles */
/* snx_arm_fp's param without bit 30: rounding, flush-to-zero and default NaN come from the CPU's FPSCR */
#define FP_PARAM_CPU_FPSCR 0u

static bool ctr_vfp_decode(uint32_t word, SnxArmVector* v) {
    if (snx_a32_decode_profile(SNX_ARM_PROFILE_ARM11, word) != SNX_A32_VECTOR || !snx_arm_vector_decode(word, v))
        return false;
    switch (v->op) {
        case AV_FMLA:
        case AV_FMLS:
        case AV_FNMLA:
        case AV_FNMLS:
        case AV_FMUL:
        case AV_FNMUL:
        case AV_FADD:
        case AV_FSUB:
        case AV_FDIV:
        case AV_MOV:
        case AV_FABS:
        case AV_FNEG:
        case AV_FSQRT: return true;
        default: return false; /* compares and conversions run in the emitter */
    }
}

/* registers are byte offsets into d[], as the decoder gives them */
static uint64_t ctr_vfp_get(const SnxArmCPU* cpu, unsigned offset, unsigned width) {
    uint64_t pair = cpu->d[offset / 8];
    return width == 64 ? pair : pair >> (offset % 8 * 8) & 0xffffffffu;
}

static void ctr_vfp_set(SnxArmCPU* cpu, unsigned offset, unsigned width, uint64_t value) {
    uint64_t* pair = &cpu->d[offset / 8];
    if (width == 64) {
        *pair = value;
        return;
    }
    unsigned shift = offset % 8 * 8;
    *pair = (*pair & ~(0xffffffffull << shift)) | (value & 0xffffffffu) << shift;
}

/* the register i steps after first, wrapping inside first's bank */
static unsigned ctr_vfp_step(unsigned first, unsigned i, unsigned step) {
    return first / BANK_BYTES * BANK_BYTES + (first + i * step) % BANK_BYTES;
}

static bool ctr_vfp_short(SnxArmCPU* cpu, uint32_t word) {
    SnxArmVector v;
    if ((cpu->cpsr & SNX_ARM_T) || !ctr_vfp_decode(word, &v)) return false;
    /* an enabled exception has a trap and flags this backend doesn't model, and the emitter stopped for it */
    if (cpu->fpscr & SNX_ARM_FPSCR_TRAP_ENABLES) return false;
    uint32_t len = (cpu->fpscr >> FPSCR_LEN_SHIFT) & 7, stride = (cpu->fpscr >> FPSCR_STRIDE_SHIFT) & 3;
    if (!(cpu->fpscr & SNX_ARM_FPSCR_LEN_STRIDE) || (stride != 0 && stride != 3)) return false;
    uint32_t count = len + 1;
    stride = stride == 3 ? 2 : 1;
    bool unary = v.op == AV_MOV || v.op == AV_FABS || v.op == AV_FNEG || v.op == AV_FSQRT;
    /* a destination in the first bank makes the operation scalar. Otherwise the vector may not revisit a register of
       its bank, and a first source in the first bank is not modelled. A unary operation has no first source */
    if (v.d < BANK_BYTES)
        count = 1;
    else if ((!unary && v.n < BANK_BYTES) || count * stride * v.width / 8 > BANK_BYTES)
        return false;
    unsigned step = v.width / 8 * stride;
    bool broadcast = v.m < BANK_BYTES; /* a second source, or the only source of a unary operation, in the first bank */
    for (unsigned i = 0; i < count; ++i) {
        unsigned d = ctr_vfp_step(v.d, i, step);
        uint64_t y = ctr_vfp_get(cpu, broadcast ? v.m : ctr_vfp_step(v.m, i, step), v.width);
        uint64_t x = unary ? y : ctr_vfp_get(cpu, ctr_vfp_step(v.n, i, step), v.width);
        uint64_t add = ctr_vfp_get(cpu, d, v.width);
        ctr_vfp_set(cpu, d, v.width, snx_arm_fp(cpu, x, y, add, v.op, v.width, FP_PARAM_CPU_FPSCR));
    }
    return true;
}

static void ctr_undefined(SnxArmCPU* cpu, uint32_t word, uint32_t reason) {
    if (reason == SNX_ARM_BACKEND_UNSUPPORTED && ctr_vfp_short(cpu, word)) {
        cpu->exit = SNX_ARM_OK;
        cpu->syndrome = 0;
        return;
    }
    cpu->exit = reason;
    cpu->syndrome = word;
}

static const SnxArmRt g_ctr_arm_rt = {ctr_undefined, NULL};

static uint32_t access_memory(void* context, uint32_t op, uint32_t va, uint32_t size, void* bytes) {
    SnxCtrCpu* cpu = context;
    if (!cpu || !size || size > 128 || !bytes) return 1;
    SnxCtrAddressSpace* space = cpu->space;
    SnxCtrMemResult result;
    if (op == (SNX_ARM_ACCESS_READ | SNX_ARM_ACCESS_EXCLUSIVE) ||
        op == (SNX_ARM_ACCESS_WRITE | SNX_ARM_ACCESS_EXCLUSIVE)) {
        if ((size != 1 && size != 2 && size != 4 && size != 8) || (va & (size - 1))) return 1;
        if (op & SNX_ARM_ACCESS_READ) {
            cpu->reservation_epoch = 0;
            if (snx_ctr_read_ram(space, va, bytes, size) != SNX_CTR_MEM_OK) return 1;
            cpu->reservation_address = va;
            cpu->reservation_size = size;
            cpu->reservation_epoch = snx_ctr_memory_epoch(space);
            return 0;
        }
        uint64_t epoch = cpu->reservation_epoch;
        cpu->reservation_epoch = 0;
        if (!epoch || epoch == UINT64_MAX || epoch != snx_ctr_memory_epoch(space) || cpu->reservation_address != va ||
            cpu->reservation_size != size)
            return SNX_ARM_EXCLUSIVE_FAILED;
        return snx_ctr_write_ram(space, va, bytes, size) == SNX_CTR_MEM_OK ? 0 : 1;
    }
    switch (op) {
        case SNX_ARM_ACCESS_READ: result = snx_ctr_read(space, va, bytes, size); break;
        case SNX_ARM_ACCESS_WRITE: result = snx_ctr_write(space, va, bytes, size); break;
        case SNX_ARM_ACCESS_FETCH: result = snx_ctr_fetch(space, va, bytes, size); break;
        case SNX_ARM_ACCESS_READ | SNX_ARM_ACCESS_MULTI: result = snx_ctr_read_ram(space, va, bytes, size); break;
        case SNX_ARM_ACCESS_WRITE | SNX_ARM_ACCESS_MULTI: result = snx_ctr_write_ram(space, va, bytes, size); break;
        default: return 1;
    }
    return result == SNX_CTR_MEM_OK ? 0 : 1;
}
bool snx_ctr_cpu_init(SnxCtrCpu* cpu, SnxCtrAddressSpace* space, uint32_t pc, uint32_t sp, bool thumb) {
    if (!cpu || !space || (pc & (thumb ? 1u : 3u)) || (sp & 7)) return false;
    *cpu = (SnxCtrCpu){0};
    cpu->state.cpu.cpsr = 0x10u | (thumb ? SNX_ARM_T : 0);
    cpu->state.cpu.r[15] = pc;
    cpu->state.cpu.r[13] = sp;
    cpu->state.cpu.rt = &g_ctr_arm_rt;
    cpu->state.abi = SNX_ARM_CHECKED_ABI;
    cpu->space = space;
    cpu->state.context = cpu;
    cpu->state.access = access_memory;
    /* a flat space: compiled code reads and writes it directly (SNX_ARM_MEMORY_FLAT), and a host fault there is the
       guest's data abort (memory/host.c) */
    cpu->state.cpu.mem = snx_ctr_space_base(space);
    cpu->state.cpu.mem_size = cpu->state.cpu.mem ? CTR_HOST_SPACE : 0;
    return true;
}
