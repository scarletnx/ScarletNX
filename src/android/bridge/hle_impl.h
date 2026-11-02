/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_ANDROID_HLE_IMPL_H
#define SNX_ANDROID_HLE_IMPL_H
#include "common/cpu/aarch64/cpu.h"
#include <stdint.h>
#include <stdio.h>

/* What the HLE libraries share (hle_*.c): a guest call's arguments and results in its CPU state */
#define X(n)   (c->x[n])
#define PTR(n) ((void*)(uintptr_t)c->x[n])
#define STR(n) ((const char*)(uintptr_t)c->x[n])

/* the host function at fn called with x0 to x7, its result in x0 (BridgeHandler with the function as data) */
void hle_pass(CPU* c, void* fn);
/* A host function with any C signature of up to 6 integer or pointer and 8 floating arguments, called with the guest's
   x0 to x5 and v0 to v7, the result into x0 or v0 by its kind ('I' an integer or a pointer, 'F' a float, 'D' a
   double, 'V' none). AAPCS64 and x86-64's System V ABI give the integers and the floating values each their own
   registers in order, so a callee takes the arguments it has from the registers it reads. A float is the low half of
   its register on both, and each floating register passes whole, so a float passes as it is. An x86-64 or AArch64
   host only */
void hle_call(CPU* c, void* fn, char kind);
/* the n'th floating argument, and the floating result */
double hle_d(const CPU* c, unsigned n);
float hle_f(const CPU* c, unsigned n);
void hle_set_d(CPU* c, double value);
void hle_set_f(CPU* c, float value);
/* the host FILE of a guest FILE*: the host's own, or stdin, stdout or stderr for bionic's __sF[0..2] */
FILE* hle_file(uint64_t guest);
/* functions whose bionic form the host's C library lacks or differs from */
size_t hle_strlcpy(char* dst, const char* src, size_t size);
int hle_strerror_r(int error, char* buffer, size_t size);
char* hle_basename(const char* path);
#endif
