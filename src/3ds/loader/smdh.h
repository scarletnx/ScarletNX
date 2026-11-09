/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The 3DS title's SMDH (ExeFS:/icon): its short title in each language and its two icons. The launcher shows the title
   and the 48x48 icon, as the console's Home Menu does */
#ifndef SNX_CTR_SMDH_H
#define SNX_CTR_SMDH_H
#include <stddef.h>

/* whether data is an SMDH: its magic, and the whole of it, the two icons included */
int snx_ctr_smdh_valid(const unsigned char* smdh, size_t size);
/* the SMDH's short title as UTF-8: English first, then Japanese, then the other languages in order; "" when none has
   one, and always ended. out_size is its room, the NUL included */
void snx_ctr_smdh_title(const unsigned char* smdh, size_t size, char* out, size_t out_size);
/* The SMDH's 48x48 icon as RGBA, 48 * 48 * 4 bytes, rows top down: 8x8 tiles in linear order, Morton order in each
   tile, RGB565 (3dbrew, SMDH#Icon graphics). 0 when written, 1 when the SMDH or the icon is not there */
int snx_ctr_smdh_icon(const unsigned char* smdh, size_t size, unsigned char* rgba);
#endif
