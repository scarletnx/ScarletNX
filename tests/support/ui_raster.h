/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The on-screen layer's quads (runtime/platform/ui.h) drawn on the CPU into a picture, as tests/vulkan_ui.inc blends
   them to compare against the GPU: a picture of a dialog for a test to look at or save, without a device */
#ifndef SNX_TEST_UI_RASTER_H
#define SNX_TEST_UI_RASTER_H
#include "nx/runtime/platform/ui.h"

/* n quads from q, in order, over a picture of w x h R G B A bytes filled with back first. atlas: ui_atlas_make's */
void ui_raster(const SnxUiQuad* q, int n, const u32* atlas, int atlas_h, u8* rgba, int w, int h, const u8 back[4]);
#endif
