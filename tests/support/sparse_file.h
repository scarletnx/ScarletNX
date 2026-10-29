/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_TEST_SPARSE_FILE_H
#define SNX_TEST_SPARSE_FILE_H
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/* Sets f's size to `size` without writing the extension. False only when sparse fixtures are unsupported, and any
   other host failure aborts the test instead of skipping it. */
bool test_sparse_resize(FILE* f, uint64_t size);

#endif
