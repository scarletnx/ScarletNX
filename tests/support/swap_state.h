/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The guest states' numbers byte-reversed, for a big-endian back end's code run on a little-endian test (host_unicorn.h's
   hostuc_swapper): the code reads the state's fields as its host's numbers. A64's vector registers hold guest bytes in
   memory's order on either host, so they stay as they are; guest memory is little-endian on both */
#ifndef SNX_TEST_SWAP_STATE_H
#define SNX_TEST_SWAP_STATE_H
#include "common/utilities/bits.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static inline void snx_test_sw32(void* p) {
    uint32_t v;
    memcpy(&v, p, 4);
    v = snx_bswap32(v);
    memcpy(p, &v, 4);
}
static inline void snx_test_sw64(void* p) {
    uint64_t v;
    memcpy(&v, p, 8);
    v = snx_bswap64(v);
    memcpy(p, &v, 8);
}
/* n doublewords (pointers, a table of addresses) */
static inline void snx_test_swap_u64s(void* p, size_t n) {
    for (size_t k = 0; k < n; ++k) snx_test_sw64((unsigned char*)p + 8 * k);
}

#ifdef SNX_TEST_SWAP_CPU /* cpu/aarch64/cpu.h's CPU and runtime/interface.h's RtAPI */
#include "common/cpu/aarch64/cpu.h"
#include "common/runtime/interface.h"
static inline void snx_test_swap_cpu(CPU* c) {
    snx_test_swap_u64s(c->x, 32);
    snx_test_sw64(&c->pc);
    snx_test_sw32(&c->n), snx_test_sw32(&c->z), snx_test_sw32(&c->cf), snx_test_sw32(&c->vf);
    snx_test_sw32(&c->fpcr), snx_test_sw32(&c->fpsr);
    snx_test_sw64(&c->tpidr), snx_test_sw64(&c->tpidrro);
    snx_test_sw64(&c->excl_addr), snx_test_sw64(&c->excl_val), snx_test_sw64(&c->excl_val2);
    snx_test_sw32(&c->excl_size), snx_test_sw32(&c->excl_on);
    snx_test_sw64(&c->mem), snx_test_sw64((void*)&c->disp), snx_test_sw64(&c->code_lo), snx_test_sw64(&c->code_hi);
    snx_test_sw64((void*)&c->rt), snx_test_sw64(&c->thread);
    snx_test_sw32((void*)&c->poll);
}
static inline void snx_test_swap_rt(RtAPI* r) { snx_test_swap_u64s(r, sizeof *r / 8); }
#endif

#ifdef SNX_TEST_SWAP_ARM /* cpu/arm/arm.h's SnxArmCPU */
#include "common/cpu/arm/arm.h"
static inline void snx_test_swap_arm(SnxArmCPU* c) {
    for (int k = 0; k < 16; ++k) snx_test_sw32(&c->r[k]);
    snx_test_sw32(&c->cpsr), snx_test_sw32(&c->exit), snx_test_sw32(&c->syndrome);
    snx_test_sw64(&c->mem), snx_test_sw64(&c->mem_size), snx_test_sw64((void*)&c->io);
    snx_test_sw64(&c->exclusive_value);
    snx_test_sw32(&c->exclusive_address), snx_test_sw32(&c->exclusive_size), snx_test_sw32(&c->exclusive_valid);
    snx_test_sw32(&c->tpidrurw), snx_test_sw32(&c->tpidruro), snx_test_sw32(&c->fpscr);
    snx_test_swap_u64s(c->d, 32);
    snx_test_sw64((void*)&c->rt);
}
#endif

#endif
