/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* dist/controls/skins, compiled in by cmake/controls_skins.cmake: each shipped skin's .cfg and PNGs */
#ifndef SNX_CONTROLS_SKINS_H
#define SNX_CONTROLS_SKINS_H
#include <stddef.h>
typedef struct {
    const char* skin; /* its folder's name, as controls_skin names it */
    const char* file; /* the file's name in it */
    const unsigned char* data;
    size_t size;
} SnxSkinFile;
extern const SnxSkinFile snx_controls_skins[];
extern const size_t snx_controls_skins_count;
#endif
