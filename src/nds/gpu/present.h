/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_NDS_PRESENT_H
#define SNX_NDS_PRESENT_H
#include "nds/board/nds.h"
/* The frame the board just finished, composed on the GPU from its 192 lines' registers and the video memory as it is
   now, and shown. Nothing when the run has no Vulkan device */
void nds_present_frame(NdsBoard* board);
#endif
