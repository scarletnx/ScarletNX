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
