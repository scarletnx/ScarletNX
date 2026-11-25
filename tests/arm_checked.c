/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "3ds/cpu/ctr_cpu.h"
#include "arm_checked_cases.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* the blocks arm_checked_emit made, the object linked with this */
#define FIXTURE_FUNCTION(id, t, p, m, pc, ...) void snx_fx_##id(SnxArmCPU* cpu);
#define FIXTURE_ADDRESS(id, t, p, m, pc, ...)  snx_fx_##id,
ARM_CHECKED_CASES(FIXTURE_FUNCTION)
static SnxArmBlock const snx_test_blocks[F_count] = {ARM_CHECKED_CASES(FIXTURE_ADDRESS)};
void snx_hosted_arm(SnxArmCPU* cpu);
void snx_hosted_thumb(SnxArmCPU* cpu);
static uint32_t const snx_test_consumed[F_count] = {
#include "arm-checked-counts.inc"
};
static SnxCtrMemory* memory;
static SnxCtrAddressSpace* space;
static SnxCtrCpu ctr;
static unsigned io_reads, io_writes;
static uint32_t io_value;
static SnxArmAccess delegated_access;
static uint32_t legacy_access(void* context, uint32_t operation, uint32_t address, uint32_t size, void* bytes) {
    if (operation & SNX_ARM_ACCESS_EXCLUSIVE) return 1;
    return delegated_access(context, operation, address, size, bytes);
}
static void put32(uint32_t va, uint32_t value) {
    unsigned char b[4];
    for (unsigned i = 0; i < 4; ++i) b[i] = (unsigned char)(value >> (i * 8));
    assert(snx_ctr_write(space, va, b, 4) == SNX_CTR_MEM_OK);
}
static uint32_t get32(uint32_t va) {
    unsigned char b[4];
    assert(snx_ctr_read(space, va, b, 4) == SNX_CTR_MEM_OK);
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}
static bool io_read(void* ctx, uint32_t pa, void* bytes, size_t size) {
    (void)ctx;
    assert(pa == 0x10100000 && size == 4);
    ++io_reads;
    for (unsigned i = 0; i < 4; ++i) ((unsigned char*)bytes)[i] = (unsigned char)(io_value >> (i * 8));
    return true;
}
static bool io_write(void* ctx, uint32_t pa, const void* bytes, size_t size) {
    (void)ctx;
    assert(pa == 0x10100000 && size == 4);
    ++io_writes;
    const unsigned char* b = bytes;
    io_value = (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
    return true;
}
static SnxArmCPU* start(unsigned id) {
    unsigned char zero[8192] = {0};
    const ArmFixture* f = &arm_fixtures[id];
    if (id == F_thumb_page_bl) assert(snx_ctr_map(space, 0x2000, 0x20001000, 4096, 7) == SNX_CTR_MEM_OK);
    assert(snx_ctr_protect(space, 0x1000, 4096, 7) == SNX_CTR_MEM_OK);
    assert(snx_ctr_protect(space, 0x4000, 8192, 3) == SNX_CTR_MEM_OK);
    assert(snx_ctr_write(space, 0x1000, zero, 4096) == SNX_CTR_MEM_OK);
    assert(snx_ctr_write(space, 0x4000, zero, 8192) == SNX_CTR_MEM_OK);
    for (size_t i = 0; i < f->count; ++i) {
        unsigned char b[4];
        unsigned n = f->thumb ? 2 : 4;
        for (unsigned j = 0; j < n; ++j) b[j] = (unsigned char)(f->code[i] >> (j * 8));
        assert(snx_ctr_write(space, f->pc + (uint32_t)i * n, b, n) == SNX_CTR_MEM_OK);
    }
    assert(snx_ctr_protect(space, 0x1000, 4096, 5) == SNX_CTR_MEM_OK);
    assert(snx_ctr_cpu_init(&ctr, space, f->pc, 0x6000, f->thumb));
    SnxArmCPU* c = &ctr.state.cpu;
    /* Poison flat memory. Any unintended flat access is immediately fatal. */
    c->mem = (uint8_t*)(uintptr_t)1;
    c->mem_size = UINT64_MAX;
    c->r[0] = 0x4000;
    c->r[1] = 0x12345678;
    c->r[2] = 0xaabbccdd;
    c->r[3] = 0x87654321;
    io_reads = io_writes = 0;
    return c;
}
/* a block is generated code, with no type signature before its entry, which the function sanitizer's check of an
   indirect call reads: the call is unchecked, as exec.c's run_block */
[[clang::no_sanitize("function")]] static void call(SnxArmBlock block, SnxArmCPU* cpu) { block(cpu); }
static void run(unsigned id, SnxArmExit exit) {
    call(snx_test_blocks[id], &ctr.state.cpu);
    if (ctr.state.cpu.exit != (uint32_t)exit) {
        fprintf(stderr, "%s: exit %u expected %u, pc=%08x syndrome=%08x\n", arm_fixtures[id].name, ctr.state.cpu.exit,
                exit, ctr.state.cpu.r[15], ctr.state.cpu.syndrome);
        assert(0);
    }
}
int main(void) {
    memory = snx_ctr_memory_new(SNX_CTR_OLD);
    assert(memory);
    space = snx_ctr_space_new(memory);
    assert(space);
    assert(snx_ctr_map(space, 0x1000, 0x20000000, 4096, 7) == SNX_CTR_MEM_OK);
    assert(snx_ctr_map(space, 0x4000, 0x20002000, 8192, 3) == SNX_CTR_MEM_OK);
    assert(snx_ctr_map(space, 0x9000, 0x20002000, 4096, 3) == SNX_CTR_MEM_OK);
    assert(snx_ctr_map(space, 0xa000, 0x10100000, 4096, 3) == SNX_CTR_MEM_OK);
    snx_ctr_memory_mmio(memory, NULL, io_read, io_write);
    SnxArmCPU* c = start(F_nop);
    run(F_nop, SNX_ARM_OK);
    assert(c->r[15] == 0x1004);
    c = start(F_nop);
    ctr.state.abi = 99;
    run(F_nop, SNX_ARM_BAD_ENTRY);
    c = start(F_nop);
    ctr.state.access = NULL;
    run(F_nop, SNX_ARM_BAD_ENTRY);
    c = start(F_nop);
    c->r[15] += 4;
    run(F_nop, SNX_ARM_BAD_ENTRY);
    c = start(F_nop);
    c->cpsr |= SNX_ARM_T;
    run(F_nop, SNX_ARM_BAD_ENTRY);
    c = start(F_nop);
    c->cpsr |= SNX_ARM_E;
    run(F_nop, SNX_ARM_ENDIAN_UNSUPPORTED);
    c = start(F_nop);
    assert(snx_ctr_protect(space, 0x1000, 4096, 1) == SNX_CTR_MEM_OK);
    run(F_nop, SNX_ARM_PREFETCH_ABORT);
    assert(c->r[15] == 0x1000 && c->syndrome == 0x1000);
    c = start(F_nop);
    assert(snx_ctr_unmap(space, 0x1000, 4096) == SNX_CTR_MEM_OK);
    run(F_nop, SNX_ARM_PREFETCH_ABORT);
    assert(c->r[15] == 0x1000);
    assert(snx_ctr_map(space, 0x1000, 0x20000000, 4096, 7) == SNX_CTR_MEM_OK);
    c = start(F_nop);
    assert(snx_ctr_protect(space, 0x1000, 4096, 4) == SNX_CTR_MEM_OK);
    run(F_nop, SNX_ARM_OK); /* execute without data-read permission */
    c = start(F_nop);
    assert(snx_ctr_protect(space, 0x1000, 4096, 7) == SNX_CTR_MEM_OK);
    put32(0x1000, 0xe3a00000);
    run(F_nop, SNX_ARM_CODE_CHANGED);
    assert(c->r[0] == 0x4000);

    c = start(F_store);
    run(F_store, SNX_ARM_OK);
    assert(get32(0x9000) == c->r[1] && c->r[0] == 0x4004);
    c = start(F_load);
    put32(0x9000, 0x98765432);
    run(F_load, SNX_ARM_OK);
    assert(c->r[2] == 0x98765432 && c->r[0] == 0x4004);
    c = start(F_store);
    assert(snx_ctr_protect(space, 0x4000, 4096, 1) == SNX_CTR_MEM_OK);
    run(F_store, SNX_ARM_DATA_ABORT);
    assert(c->r[0] == 0x4000 && get32(0x4000) == 0);
    c = start(F_load);
    c->r[0] = 0xfffffffe;
    run(F_load, SNX_ARM_DATA_ABORT);
    assert(c->r[0] == 0xfffffffe && c->r[2] == 0xaabbccdd);
    c = start(F_store);
    c->r[0] = 0x4001;
    run(F_store, SNX_ARM_OK);
    assert(get32(0x4001) == 0x12345678);
    c = start(F_stm);
    run(F_stm, SNX_ARM_OK);
    assert(c->r[0] == 0x4008 && get32(0x4004) == 0xaabbccdd);
    c = start(F_ldm);
    put32(0x4000, 0x11223344);
    put32(0x4004, 0x55667788);
    run(F_ldm, SNX_ARM_OK);
    assert(c->r[0] == 0x4008 && c->r[1] == 0x11223344 && c->r[2] == 0x55667788);
    c = start(F_stm);
    c->r[0] = 0x4ffc;
    assert(snx_ctr_protect(space, 0x5000, 4096, 1) == SNX_CTR_MEM_OK);
    run(F_stm, SNX_ARM_DATA_ABORT);
    assert(c->r[0] == 0x4ffc && get32(0x4ffc) == 0);
    c = start(F_ldm);
    c->r[0] = 0x4ffc;
    put32(0x4ffc, 1);
    assert(snx_ctr_protect(space, 0x5000, 4096, 2) == SNX_CTR_MEM_OK);
    run(F_ldm, SNX_ARM_DATA_ABORT);
    assert(c->r[0] == 0x4ffc && c->r[1] == 0x12345678 && c->r[2] == 0xaabbccdd);
    c = start(F_strd);
    run(F_strd, SNX_ARM_OK);
    assert(c->r[0] == 0x4008 && get32(0x4004) == 0x87654321);
    c = start(F_ldrd);
    put32(0x4000, 5);
    put32(0x4004, 7);
    run(F_ldrd, SNX_ARM_OK);
    assert(c->r[2] == 5 && c->r[3] == 7 && c->r[0] == 0x4008);
    /* unaligned support on (SCTLR.U): a doubleword needs word alignment, not doubleword alignment */
    c = start(F_ldrd);
    c->r[0] += 4;
    put32(0x4004, 7);
    put32(0x4008, 9);
    run(F_ldrd, SNX_ARM_OK);
    assert(c->r[2] == 7 && c->r[3] == 9 && c->r[0] == 0x400c);
    c = start(F_ldrd);
    c->r[0] += 2;
    run(F_ldrd, SNX_ARM_ALIGNMENT);
    assert(c->r[0] == 0x4002);
    c = start(F_movpc11);
    c->r[0] = 0x4003;
    run(F_movpc11, SNX_ARM_OK);
    assert(c->r[15] == 0x4000 && !(c->cpsr & SNX_ARM_T));
    c = start(F_movpc57);
    c->r[0] = 0x4003;
    run(F_movpc57, SNX_ARM_OK);
    assert(c->r[15] == 0x4002 && (c->cpsr & SNX_ARM_T));
    start(F_movw11);
    run(F_movw11, SNX_ARM_UNDEFINED);
    c = start(F_movw57);
    run(F_movw57, SNX_ARM_OK);
    assert(c->r[0] == 0x1234);
    start(F_exclusive_a57);
    run(F_exclusive_a57, SNX_ARM_BACKEND_UNSUPPORTED);
    c = start(F_exclusive_pair);
    put32(0x4000, 0x11223344);
    run(F_exclusive_pair, SNX_ARM_OK);
    assert(c->r[2] == 0x11223344 && !c->r[3] && get32(0x4000) == 0x12345678 && !c->exclusive_valid);
    c = start(F_exclusive_pair);
    put32(0x4000, 0x11223344);
    run(F_exclusive, SNX_ARM_OK);
    assert(c->r[2] == 0x11223344 && c->exclusive_valid && c->exclusive_size == 4);
    put32(0x9000, 0xabcdef01);
    put32(0x9000, 0x11223344); /* physical alias ABA */
    run(F_exclusive_store, SNX_ARM_OK);
    assert(c->r[3] == 1 && get32(0x4000) == 0x11223344);
    c->r[15] = 0x1004;
    run(F_exclusive_store, SNX_ARM_OK);
    assert(c->r[3] == 1);
    c = start(F_exclusive_pair);
    run(F_exclusive, SNX_ARM_OK);
    uint64_t epoch = snx_ctr_memory_epoch(space);
    assert(snx_ctr_write(space, 0x6000, "x", 1) == SNX_CTR_MEM_UNMAPPED);
    assert(snx_ctr_memory_epoch(space) == epoch);
    run(F_exclusive_store, SNX_ARM_OK);
    assert(!c->r[3]); /* failed external write has no effect */
    c = start(F_exclusive_pair);
    run(F_exclusive, SNX_ARM_OK);
    assert(snx_ctr_protect(space, 0x5000, 4096, 3) == SNX_CTR_MEM_OK);
    run(F_exclusive_store, SNX_ARM_OK);
    assert(c->r[3] == 1 && !get32(0x4000));
    c = start(F_exclusive_pair);
    assert(snx_ctr_protect(space, 0x4000, 4096, 1) == SNX_CTR_MEM_OK);
    run(F_exclusive, SNX_ARM_OK);
    run(F_exclusive_store, SNX_ARM_DATA_ABORT);
    assert(c->r[3] == 0x87654321 && !get32(0x4000) && !c->exclusive_valid);
    c = start(F_exclusive);
    c->r[0] = 0x4001;
    run(F_exclusive, SNX_ARM_ALIGNMENT);
    assert(c->r[2] == 0xaabbccdd && !c->exclusive_valid);
    c = start(F_exclusive);
    c->r[0] = 0x6000;
    run(F_exclusive, SNX_ARM_DATA_ABORT);
    assert(c->r[2] == 0xaabbccdd && !c->exclusive_valid);
    c = start(F_exclusive);
    c->r[0] = 0xa000;
    run(F_exclusive, SNX_ARM_DATA_ABORT);
    assert(!io_reads);
    c = start(F_exclusive_pair);
    run(F_exclusive, SNX_ARM_OK);
    SnxCtrCpu peer;
    assert(snx_ctr_cpu_init(&peer, space, 0x1000, 0x6000, false));
    peer.state.cpu.r[0] = 0x4000;
    peer.state.cpu.r[1] = 99;
    call(snx_test_blocks[F_exclusive_pair], &peer.state.cpu);
    assert(peer.state.cpu.exit == SNX_ARM_OK && !peer.state.cpu.r[3] && get32(0x4000) == 99);
    run(F_exclusive_store, SNX_ARM_OK);
    assert(c->r[3] == 1 && get32(0x4000) == 99);
    SnxCtrAddressSpace* alias = snx_ctr_space_new(memory);
    assert(alias);
    assert(snx_ctr_map(alias, 0x10000, 0x20002000, 4096, 3) == SNX_CTR_MEM_OK);
    c = start(F_exclusive_pair);
    run(F_exclusive, SNX_ARM_OK);
    assert(snx_ctr_write_ram(alias, 0x10000, "\0\0\0\0", 4) == SNX_CTR_MEM_OK);
    run(F_exclusive_store, SNX_ARM_OK);
    assert(c->r[3] == 1);
    snx_ctr_space_free(alias);
    c = start(F_exclusive_clear);
    run(F_exclusive_clear, SNX_ARM_OK);
    assert(c->r[3] == 1 && !get32(0x4000));
    c = start(F_exclusive_local);
    run(F_exclusive_local, SNX_ARM_OK);
    assert(c->r[3] == 1 && get32(0x4000) == 0x12345678);
    c = start(F_exclusive_byte);
    put32(0x4000, 0x11223344);
    run(F_exclusive_byte, SNX_ARM_OK);
    assert(c->r[2] == 0x44 && !c->r[3] && get32(0x4000) == 0x11223378);
    c = start(F_exclusive_half);
    put32(0x4000, 0x11223344);
    run(F_exclusive_half, SNX_ARM_OK);
    assert(c->r[2] == 0x3344 && !c->r[3] && get32(0x4000) == 0x11225678);
    c = start(F_exclusive_double);
    put32(0x4000, 0x11223344);
    put32(0x4004, 0x55667788);
    run(F_exclusive_double, SNX_ARM_OK);
    assert(!c->r[4] && get32(0x4000) == 0x11223345 && get32(0x4004) == 0x55667788);
    c = start(F_exclusive_double);
    c->r[0] = 0x4004;
    run(F_exclusive_double, SNX_ARM_ALIGNMENT);
    c = start(F_exclusive);
    delegated_access = ctr.state.access;
    ctr.state.access = legacy_access;
    run(F_exclusive, SNX_ARM_DATA_ABORT);
    assert(c->r[2] == 0xaabbccdd && !c->exclusive_valid);
    c = start(F_exclusive_pair);
    run(F_exclusive, SNX_ARM_OK);
    delegated_access = ctr.state.access;
    ctr.state.access = legacy_access;
    run(F_exclusive_store, SNX_ARM_DATA_ABORT);
    assert(c->r[3] == 0x87654321 && !get32(0x4000));
    c = start(F_vfp_store);
    c->d[0] = 0x1122334455667788ull;
    run(F_vfp_store, SNX_ARM_OK);
    assert(get32(0x4000) == 0x55667788 && get32(0x4004) == 0x11223344 && c->r[0] == 0x4000);
    c = start(F_vfp_load);
    c->r[0] = 0x4004;
    put32(0x4004, 0x55667788);
    put32(0x4008, 0x11223344);
    run(F_vfp_load, SNX_ARM_OK);
    assert(c->d[0] == 0x1122334455667788ull);
    for (unsigned id = F_vfp_load; id <= F_vfp_store; ++id) {
        c = start(id);
        c->r[0] = 0x5ffc;
        c->d[0] = 0x1122334455667788ull;
        put32(0x5ffc, 0xaabbccdd);
        run(id, SNX_ARM_DATA_ABORT);
        assert(c->r[0] == 0x5ffc && c->d[0] == 0x1122334455667788ull && get32(0x5ffc) == 0xaabbccdd);
        c = start(id);
        c->r[0] = 0xa000;
        run(id, SNX_ARM_DATA_ABORT);
        assert(!io_reads && !io_writes && c->r[0] == 0xa000);
    }
    c = start(F_vfp_store);
    c->r[0] = 0x4ffc;
    c->d[0] = 0x1122334455667788ull;
    put32(0x4ffc, 0xaabbccdd);
    put32(0x5000, 0x12345678);
    assert(snx_ctr_protect(space, 0x5000, 4096, 1) == SNX_CTR_MEM_OK);
    run(F_vfp_store, SNX_ARM_DATA_ABORT);
    assert(get32(0x4ffc) == 0xaabbccdd && get32(0x5000) == 0x12345678);
    c = start(F_vfp_multi);
    c->r[0] = 0x5fe0;
    for (unsigned i = 0; i < 8; ++i) c->d[i] = 0x11223344 + i;
    run(F_vfp_multi, SNX_ARM_DATA_ABORT);
    assert(c->r[0] == 0x5fe0);
    for (unsigned i = 0; i < 8; ++i) assert(c->d[i] == 0x11223344 + i);
    c = start(F_vfp_multi);
    run(F_vfp_multi, SNX_ARM_OK);
    assert(c->r[0] == 0x4040);
    start(F_vfp);
    run(F_vfp, SNX_ARM_OK);
    start(F_neon);
    run(F_neon, SNX_ARM_UNDEFINED);
    c = start(F_condstore);
    c->r[0] = 0xdead0000;
    run(F_condstore, SNX_ARM_OK);
    assert(c->r[15] == 0x1004); /* skipped data access, but fetch is mandatory */
    start(F_condstore);
    assert(snx_ctr_protect(space, 0x1000, 4096, 1) == SNX_CTR_MEM_OK);
    run(F_condstore, SNX_ARM_PREFETCH_ABORT);
    c = start(F_smc);
    assert(snx_ctr_protect(space, 0x1000, 4096, 7) == SNX_CTR_MEM_OK);
    c->r[0] = 0x1004;
    c->r[1] = 0xe3a02009;
    run(F_smc, SNX_ARM_CODE_CHANGED);
    assert(c->r[2] == 0xaabbccdd && c->r[15] == 0x1004);
    c = start(F_svc);
    run(F_svc, SNX_ARM_SVC);
    assert(c->syndrome == 9 && c->r[15] == 0x1004);
    start(F_barrier);
    run(F_barrier, SNX_ARM_OK);

    c = start(F_mmio_store);
    c->r[0] = 0xa000;
    run(F_mmio_store, SNX_ARM_OK);
    assert(io_writes == 1 && io_value == 0x12345678);
    c = start(F_mmio_load);
    c->r[0] = 0xa000;
    run(F_mmio_load, SNX_ARM_OK);
    assert(io_reads == 1 && c->r[2] == 0x12345678);
    c = start(F_mmio_store);
    c->r[0] = 0xa001;
    run(F_mmio_store, SNX_ARM_DATA_ABORT);
    assert(!io_writes);
    c = start(F_stm);
    c->r[0] = 0xa000;
    run(F_stm, SNX_ARM_DATA_ABORT);
    assert(!io_writes && c->r[0] == 0xa000);
    c = start(F_ldm);
    c->r[0] = 0xa000;
    run(F_ldm, SNX_ARM_DATA_ABORT);
    assert(!io_reads && c->r[0] == 0xa000);

    c = start(F_thumb_bl);
    run(F_thumb_bl, SNX_ARM_OK);
    assert(c->r[15] == 0x401002 && c->r[14] == 0x1005);
    c = start(F_thumb_bl_neg);
    run(F_thumb_bl_neg, SNX_ARM_OK);
    assert(c->r[15] == 0xffc01004 && c->r[14] == 0x1005);
    c = start(F_thumb_prefix);
    run(F_thumb_prefix, SNX_ARM_OK);
    assert(c->r[15] == 0x1002 && c->r[14] == 0xffc01004);
    c = start(F_thumb_page_bl);
    assert(snx_ctr_unmap(space, 0x2000, 4096) == SNX_CTR_MEM_OK);
    run(F_thumb_page_bl, SNX_ARM_PREFETCH_ABORT);
    assert(c->r[15] == 0x2000 && c->r[14] == 0x2002 && c->syndrome == 0x2000);
    c = start(F_thumb_suffix);
    c->r[14] = 0x4000;
    run(F_thumb_suffix, SNX_ARM_OK);
    assert(c->r[15] == 0x4006 && c->r[14] == 0x1003);
    c = start(F_thumb_blx);
    run(F_thumb_blx, SNX_ARM_OK);
    assert(c->r[15] == 0x1008 && !(c->cpsr & SNX_ARM_T));
    c = start(F_thumb_pc);
    run(F_thumb_pc, SNX_ARM_OK);
    assert(c->r[0] == 0x1006);
    c = start(F_thumb_addpc);
    c->r[0] = 1;
    run(F_thumb_addpc, SNX_ARM_OK);
    assert(c->r[15] == 0x1006 && c->r[0] == 1 && snx_test_consumed[F_thumb_addpc] == 1);
    c = start(F_thumb_literal);
    assert(snx_ctr_protect(space, 0x1000, 4096, 7) == SNX_CTR_MEM_OK);
    put32(0x1004, 0x11223344);
    run(F_thumb_literal, SNX_ARM_OK);
    assert(c->r[0] == 0x11223344);
    c = start(F_thumb_adr);
    run(F_thumb_adr, SNX_ARM_OK);
    assert(c->r[0] == 0x1004);
    c = start(F_thumb_push);
    c->r[14] = 0x8765;
    run(F_thumb_push, SNX_ARM_OK);
    assert(c->r[13] == 0x5ff4 && get32(0x5ff4) == 0x4000 && get32(0x5ffc) == 0x8765);
    c = start(F_thumb_push);
    c->r[13] = 0x5004;
    assert(snx_ctr_protect(space, 0x5000, 4096, 1) == SNX_CTR_MEM_OK);
    run(F_thumb_push, SNX_ARM_DATA_ABORT);
    assert(c->r[13] == 0x5004 && get32(0x4ff8) == 0);
    c = start(F_thumb_pop);
    c->r[13] = 0x4000;
    put32(0x4000, 11);
    put32(0x4004, 13);
    put32(0x4008, 0x2001);
    run(F_thumb_pop, SNX_ARM_OK);
    assert(c->r[0] == 11 && c->r[1] == 13 && c->r[13] == 0x400c && c->r[15] == 0x2000 && (c->cpsr & SNX_ARM_T));
    c = start(F_thumb_ldm);
    c->r[1] = 0x4000;
    put32(0x4000, 17);
    put32(0x4004, 19);
    run(F_thumb_ldm, SNX_ARM_OK);
    assert(c->r[0] == 17 && c->r[1] == 19);
    c = start(F_thumb_rev);
    run(F_thumb_rev, SNX_ARM_OK);
    assert(c->r[0] == 0x78563412);
    start(F_thumb_hint);
    run(F_thumb_hint, SNX_ARM_UNDEFINED);
    c = start(F_thumb_bkpt);
    run(F_thumb_bkpt, SNX_ARM_BREAKPOINT);
    assert(c->syndrome == 0xaa && c->r[15] == 0x1002);
    c = start(F_thumb_svc);
    run(F_thumb_svc, SNX_ARM_SVC);
    assert(c->syndrome == 7 && c->r[15] == 0x1002);
    c = start(F_thumb_store);
    c->r[1] = 0x4000;
    run(F_thumb_store, SNX_ARM_OK);
    assert(get32(0x4000) == 0xaabbccdd);
    c = start(F_thumb_load);
    c->r[1] = 0x4000;
    put32(0x4000, 23);
    run(F_thumb_load, SNX_ARM_OK);
    assert(c->r[2] == 23);
    /* hosted blocks, for a runtime that owns the process: no entry checks, and SETEND BE stops one */
    for (unsigned elsewhere = 0; elsewhere < 2; ++elsewhere) {
        SnxArmCPU hosted = {0};
        hosted.r[15] = elsewhere ? 0x7000 : 0x1000;
        hosted.cpsr = 0x10 | (elsewhere ? SNX_ARM_T | SNX_ARM_E : 0);
        call(snx_hosted_arm, &hosted);
        assert(hosted.exit == SNX_ARM_ENDIAN_UNSUPPORTED && hosted.r[15] == 0x1004);
        hosted = (SnxArmCPU){0};
        hosted.r[15] = elsewhere ? 0x7000 : 0x2000;
        hosted.cpsr = 0x10 | (elsewhere ? 0 : SNX_ARM_T);
        call(snx_hosted_thumb, &hosted);
        assert(hosted.exit == SNX_ARM_OK && hosted.r[15] == 0x2004);
    }
    assert(!snx_ctr_cpu_init(NULL, space, 0x1000, 0x6000, false));
    assert(!snx_ctr_cpu_init(&ctr, NULL, 0x1000, 0x6000, false));
    assert(!snx_ctr_cpu_init(&ctr, space, 0x1002, 0x6000, false));
    assert(!snx_ctr_cpu_init(&ctr, space, 0x1000, 0x6004, false));
    snx_ctr_space_free(space);
    snx_ctr_memory_free(memory);
    puts("static ARM11/checked-memory AOT witnesses passed (no LLVM in executable)");
    return 0;
}
