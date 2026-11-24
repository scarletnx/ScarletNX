/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* <string.h> for a test's code compiled for a machine that is not the host's, whose C library is not here: what
   common/utilities/bits.h and endian.h name of it */
#ifndef SNX_TEST_FREESTANDING_STRING_H
#define SNX_TEST_FREESTANDING_STRING_H
#include <stddef.h>
void* memcpy(void* to, const void* from, size_t n);
void* memset(void* to, int value, size_t n);
int memcmp(const void* a, const void* b, size_t n);
size_t strlen(const char* s);
int strcmp(const char* a, const char* b);
int strncmp(const char* a, const char* b, size_t n);
#endif
