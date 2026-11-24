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
#endif
