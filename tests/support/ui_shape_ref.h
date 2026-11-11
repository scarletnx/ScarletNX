/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* ui.frag's UI_SHAPE on the CPU (runtime/platform/ui.h's ui_shape), for vulkan_ui's blend: whether the pixel whose
   middle is at px, py from the shape's top left draws, on a shape of w x h with kind, label, flags and edge as its
   quad's u, v, uw and vh hold them: 1 drawn, 0 not, -1 where the GPU's rounding could tell it either way (the middle
   within a thousandth of a pixel of the edge's or the label's bounds) */
#ifndef SNX_TEST_UI_SHAPE_REF_H
#define SNX_TEST_UI_SHAPE_REF_H

int ui_shape_ref(unsigned kind, unsigned label, unsigned flags, unsigned edge, float w, float h, float px, float py);

#endif
