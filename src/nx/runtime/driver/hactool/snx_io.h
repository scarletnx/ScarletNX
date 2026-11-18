/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Included before anything else in every hactool file (cmake/hactool.cmake). hactool runs inside scarletnx --extract,
   so what it prints, which can hold key material, goes to snx_hac_printf and never reaches the console; an exit ends
   the extraction through snx_hac_exit, which lets the driver clean up first; and what it writes to files is counted for
   the progress row. stdio.h and stdlib.h come first, so their own declarations keep their names */
#ifndef SNX_HACTOOL_IO_H
#define SNX_HACTOOL_IO_H

#include <stdio.h>
#include <stdlib.h>

int snx_hac_printf(const char *format, ...);
int snx_hac_fprintf(FILE *file, const char *format, ...);
size_t snx_hac_fwrite(const void *data, size_t size, size_t count, FILE *file);
[[noreturn]] void snx_hac_exit(int status);

#define printf snx_hac_printf
#define fprintf snx_hac_fprintf
#define fwrite snx_hac_fwrite
#define exit snx_hac_exit

#endif
