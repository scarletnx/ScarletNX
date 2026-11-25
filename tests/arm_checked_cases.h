/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_TEST_ARM_CHECKED_CASES_H
#define SNX_TEST_ARM_CHECKED_CASES_H
#include "common/cpu/arm/checked.h"
/* id, Thumb, ISA, checked, PC, words or halfwords */
#define ARM_CHECKED_CASES(F)                                                                                           \
    F(nop, 0, 1, 1, 0x1000, 0xe1a00000)                                                                                \
    F(store, 0, 1, 1, 0x1000, 0xe4801004)                                                                              \
    F(load, 0, 1, 1, 0x1000, 0xe4902004)                                                                               \
    F(stm, 0, 1, 1, 0x1000, 0xe8a00006)                                                                                \
    F(ldm, 0, 1, 1, 0x1000, 0xe8b00006)                                                                                \
    F(strd, 0, 1, 1, 0x1000, 0xe0c020f8)                                                                               \
    F(ldrd, 0, 1, 1, 0x1000, 0xe0c020d8)                                                                               \
    F(movpc11, 0, 1, 1, 0x1000, 0xe1a0f000)                                                                            \
    F(movpc57, 0, 0, 0, 0x1000, 0xe1a0f000)                                                                            \
    F(movw11, 0, 1, 1, 0x1000, 0xe3010234)                                                                             \
    F(movw57, 0, 0, 1, 0x1000, 0xe3010234)                                                                             \
    F(exclusive, 0, 1, 1, 0x1000, 0xe1902f9f)                                                                          \
    F(exclusive_a57, 0, 0, 1, 0x1000, 0xe1902f9f)                                                                      \
    F(exclusive_pair, 0, 1, 1, 0x1000, 0xe1902f9f, 0xe1803f91)                                                         \
    F(exclusive_store, 0, 1, 1, 0x1004, 0xe1803f91)                                                                    \
    F(exclusive_byte, 0, 1, 1, 0x1000, 0xe1d02f9f, 0xe1c03f91)                                                         \
    F(exclusive_half, 0, 1, 1, 0x1000, 0xe1f02f9f, 0xe1e03f91)                                                         \
    F(exclusive_double, 0, 1, 1, 0x1000, 0xe1b02f9f, 0xe2822001, 0xe1a04f92)                                           \
    F(exclusive_clear, 0, 1, 1, 0x1000, 0xe1902f9f, 0xf57ff01f, 0xe1803f91)                                            \
    F(exclusive_local, 0, 1, 1, 0x1000, 0xe1902f9f, 0xe5801000, 0xe1803f91)                                      \
    F(vfp_load, 0, 1, 1, 0x1000, 0xed900b00)                                                                           \
    F(vfp_store, 0, 1, 1, 0x1000, 0xed800b00)                                                                          \
    F(vfp_multi, 0, 1, 1, 0x1000, 0xecb00b10)                                                                          \
    F(vfp, 0, 1, 1, 0x1000, 0xee300a01)                                                                                \
    F(neon, 0, 1, 1, 0x1000, 0xf2010802)                                                                               \
    F(condstore, 0, 1, 1, 0x1000, 0x05801000)                                                                          \
    F(smc, 0, 1, 1, 0x1000, 0xe5801000, 0xe3a02007)                                                                    \
    F(svc, 0, 1, 1, 0x1000, 0xef000009)                                                                                \
    F(mmio_store, 0, 1, 1, 0x1000, 0xe5801000)                                                                         \
    F(mmio_load, 0, 1, 1, 0x1000, 0xe5902000)                                                                          \
    F(barrier, 0, 1, 1, 0x1000, 0xee070fba)                                                                            \
    F(thumb_bl, 1, 1, 1, 0x1000, 0xf3ff, 0xffff)                                                                       \
    F(thumb_bl_neg, 1, 1, 1, 0x1000, 0xf400, 0xf800)                                                                   \
    F(thumb_prefix, 1, 1, 1, 0x1000, 0xf400)                                                                           \
    F(thumb_page_bl, 1, 1, 1, 0x1ffe, 0xf000, 0xf800)                                                                  \
    F(thumb_suffix, 1, 1, 1, 0x1000, 0xf803)                                                                           \
    F(thumb_blx, 1, 1, 1, 0x1000, 0xf000, 0xe802)                                                                      \
    F(thumb_pc, 1, 1, 1, 0x1002, 0x4678)                                                                               \
    F(thumb_addpc, 1, 1, 1, 0x1002, 0x4487, 0x2002)                                                                    \
    F(thumb_literal, 1, 1, 1, 0x1002, 0x4800)                                                                          \
    F(thumb_adr, 1, 1, 1, 0x1002, 0xa000)                                                                              \
    F(thumb_push, 1, 1, 1, 0x1000, 0xb503)                                                                             \
    F(thumb_pop, 1, 1, 1, 0x1000, 0xbd03)                                                                              \
    F(thumb_ldm, 1, 1, 1, 0x1000, 0xc903)                                                                              \
    F(thumb_rev, 1, 1, 1, 0x1000, 0xba08)                                                                              \
    F(thumb_hint, 1, 1, 1, 0x1000, 0xbf08)                                                                             \
    F(thumb_bkpt, 1, 1, 1, 0x1000, 0xbeaa)                                                                             \
    F(thumb_svc, 1, 1, 1, 0x1000, 0xdf07)                                                                              \
    F(thumb_store, 1, 1, 1, 0x1000, 0x600a)                                                                            \
    F(thumb_load, 1, 1, 1, 0x1000, 0x680a)
#define FIXTURE_ENUM(id, t, p, m, pc, ...) F_##id,
enum { ARM_CHECKED_CASES(FIXTURE_ENUM) F_count };
#undef FIXTURE_ENUM
typedef struct {
    const char* name;
    unsigned thumb, profile, checked;
    uint32_t pc, code[8];
    size_t count;
} ArmFixture;
#define FIXTURE_DATA(id, t, p, m, pc, ...)                                                                             \
    {#id, t, p, m, pc, {__VA_ARGS__}, sizeof((uint32_t[]){__VA_ARGS__}) / sizeof(uint32_t)},
static const ArmFixture arm_fixtures[] = {ARM_CHECKED_CASES(FIXTURE_DATA)};
#undef FIXTURE_DATA
#endif
