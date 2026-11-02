/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_ANDROID_ARGS_H
#define SNX_ANDROID_ARGS_H
#include "common/cpu/aarch64/cpu.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A guest call's arguments, read in order as AAPCS64 passes them: integers and pointers in x0 to x7, floating ones in
   v0 to v7, then both on the stack in 8-byte slots. Variadic arguments go the same way on Linux and Android (unlike
   Apple's). A guest va_list (vsnprintf's) is AAPCS64's structure: __stack, __gr_top, __vr_top, __gr_offs, __vr_offs */
typedef struct {
    const CPU* c;
    unsigned next_x, next_v; /* the next register of each kind */
    uint64_t stack;          /* the next stack slot */
    /* a va_list's: the general and the vector save areas, and where each kind's next one is (negative offsets) */
    bool va;
    uint64_t gr_top, vr_top;
    int32_t gr_offs, vr_offs;
} GuestArgs;

/* the arguments from the first'th integer register on (after the named ones), the next floating one at v[vfirst] */
void args_from_registers(GuestArgs* a, const CPU* c, unsigned first, unsigned vfirst);
/* the arguments of a guest va_list at address va_list */
void args_from_va_list(GuestArgs* a, uint64_t va_list);
uint64_t args_int(GuestArgs* a);
double args_double(GuestArgs* a);

/* a printf format of the guest with its arguments, into out as vsnprintf would: the length it needs */
int guest_format(char* out, size_t size, const char* format, GuestArgs* a);
#endif
