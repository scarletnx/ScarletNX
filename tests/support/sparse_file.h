/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_TEST_SPARSE_FILE_H
#define SNX_TEST_SPARSE_FILE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Sets f's size to `size` without writing the extension. False only when sparse fixtures are unsupported, and any
   other host failure aborts the test instead of skipping it. */
bool test_sparse_resize(FILE* f, uint64_t size);

/* Moves f to `at` with the C library's own call, for fixtures that must not depend on snx_seek */
void test_seek_at(FILE* f, uint64_t at);

/* A whole-file read-only mapping, so a view is told the size its backing really has instead of one a short buffer
   would advertise */
typedef struct {
    const unsigned char* base;
    size_t size;
} TestMap;

/* False only when the host cannot map a file of `size` bytes, and any other failure aborts the test */
bool test_map_open(TestMap* map, const char* path, uint64_t size);
void test_map_close(TestMap* map);

#endif
