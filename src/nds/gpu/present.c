/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nds/gpu/present.h"
#include "nds/gpu/gpu3d.h"
#include "nds/gpu/vk/vk.h"

void nds_present_frame(NdsBoard* board) {
    if (!nds_vk()) return;
    nds_vk_frame_compose(board);
    nds_vk_present();
}

void nds_gpu3d_render(NdsBoard* board) {
    if (!nds_vk()) return;
    nds_vk_render3d(board);
}
