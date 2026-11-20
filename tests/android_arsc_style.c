/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A style applied with no array for the indices of the attributes it found (src/android/res/arsc.c): Android's
   applyStyle takes none, and writes the values alone. Over assets that hold no table every attribute is not found:
   its seven words are zeros, with the array of indices saying none when one is given */
#include "android/res/arsc.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    static const uint32_t attrs[2] = {0x01010001, 0x01010002};
    int32_t out[2 * ARSC_STYLE_ENTRIES], indices[3] = {7, 7, 7};
    ArscAssets* assets = arsc_create();
    ArscTheme* theme = assets ? arsc_theme_create(assets) : NULL;
    assert(theme);
    memset(out, 0x55, sizeof out);
    arsc_apply_style(theme, NULL, 0, 0, attrs, 2, out, NULL);
    for (unsigned i = 0; i < 2 * ARSC_STYLE_ENTRIES; ++i) assert(out[i] == 0);
    memset(out, 0x55, sizeof out);
    arsc_apply_style(theme, NULL, 0, 0, attrs, 2, out, indices);
    assert(indices[0] == 0 && indices[1] == 7 && out[0] == 0 && out[2 * ARSC_STYLE_ENTRIES - 1] == 0);
    arsc_theme_destroy(theme);
    arsc_destroy(assets);
    puts("android_arsc_style: ok");
    return 0;
}
