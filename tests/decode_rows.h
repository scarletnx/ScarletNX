/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* a pattern of a .decode file as tools/decodetree --rows writes it: its fixed bits, and the bits of each of its fields
   (0 past the last) */
#ifndef SNX_TEST_DECODE_ROWS_H
#define SNX_TEST_DECODE_ROWS_H
#include <stdint.h>
typedef struct {
    uint32_t mask, value;
    uint32_t fields[8];
} SnxDecodeRow;
#endif
