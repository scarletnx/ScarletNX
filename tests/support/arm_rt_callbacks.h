/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A runtime's SnxArmRt functions for arm_x64.c's --callee, the test's own and, compiled for the machine under test
   (arm_call_abi_callee.c), the same: each answers for its word as a compiler takes a uint32_t. The constant's low bits
   keep a compiler from comparing only the top of the register: a machine whose ABI extends a narrow argument compares
   the whole of it with the constant's extension */
#ifndef SNX_TEST_ARM_RT_CALLBACKS_H
#define SNX_TEST_ARM_RT_CALLBACKS_H
#include "common/cpu/arm/arm.h"

void snx_test_undefined(SnxArmCPU* cpu, uint32_t word, uint32_t reason);
void snx_test_undefined(SnxArmCPU* cpu, uint32_t word, uint32_t reason) {
    cpu->r[8] = word >= 0xE1234567u;
    cpu->r[9] = reason == SNX_ARM_UNDEFINED;
    cpu->r[10] = word;
}
void snx_test_coproc(SnxArmCPU* cpu, uint32_t word);
void snx_test_coproc(SnxArmCPU* cpu, uint32_t word) {
    cpu->r[8] = word >= 0xE1234567u;
    cpu->r[10] = word;
}
/* SnxArmCPU.io: a read gives a value of its width (an ARMv4T LDRSH's sign extended) and a write ends the block one time
   in four, both from the call and the state the block has then, which compiled code and the test's
   own must see alike */
uint32_t snx_test_io(SnxArmCPU* cpu, uint32_t address, uint32_t size_write, uint32_t value);
uint32_t snx_test_io(SnxArmCPU* cpu, uint32_t address, uint32_t size_write, uint32_t value) {
    uint32_t v = (address ^ size_write * 0x9E3779B1u) * 0x85EBCA6Bu + value;
    for (unsigned r = 0; r < 16; ++r) v = (v ^ cpu->r[r]) * 0xC2B2AE35u + (v >> 15);
    v ^= cpu->cpsr + (v >> 13);
    if (size_write & 0x100) return v & 3 ? 0 : v | 1;
    switch (size_write) {
        case 1: return v & 0xff;
        case 2: return v & 0xffff;
        case 0x202: return v & 0x10000 ? (uint32_t)(int32_t)(int16_t)v : (uint32_t)(int32_t)(int8_t)v;
        default: return v;
    }
}
#endif
