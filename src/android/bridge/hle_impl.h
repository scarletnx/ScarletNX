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
   its register on both, and each floating register passes whole, so a float passes as it is.
   Windows x64 passes them by position instead: params gives each argument's class in order ('I', 'F' or 'D'), and a
   function with a floating argument must have it. NULL params: integers and pointers alone (x0 to x7 there). An
   x86-64 or AArch64 host only */
void hle_call(CPU* c, void* fn, char kind, const char* params);
/* hle_call for up to 12 integer arguments: x0 to x7, then the guest's first 4 stack slots (Vulkan's commands take up
   to 11). The host's System V ABI passes the integers past its sixth on its stack in order, as the floating ones stay
   in registers; Windows' passes every argument past its fourth on its stack */
void hle_call_wide(CPU* c, void* fn, char kind, const char* params);
/* the n'th floating argument, and the floating result */
double hle_d(const CPU* c, unsigned n);
float hle_f(const CPU* c, unsigned n);
void hle_set_d(CPU* c, double value);
void hle_set_f(CPU* c, float value);
/* the host FILE of a guest FILE*: the host's own, or stdin, stdout or stderr for bionic's __sF[0..2] */
FILE* hle_file(uint64_t guest);
/* The guest's errno, in Linux's numbers as bionic's are. On Linux it is the host's own errno. Elsewhere the guest has
   its own, and each HLE call (bridge.c's call_export) sets it from the host's errno that the call left, in the
   guest's numbers: hle_errno_begin before the call, hle_errno_end after it. hle_set_errno sets it from a Linux number
   (a kernel's result, an error the adapter decides) */
int* hle_errno_location(void);
void hle_errno_begin(void);
void hle_errno_end(void);
void hle_set_errno(int linux_error);
/* functions whose bionic form the host's C library lacks or differs from */
size_t hle_strlcpy(char* dst, const char* src, size_t size);
int hle_strerror_r(int error, char* buffer, size_t size);
char* hle_basename(const char* path);
#endif
