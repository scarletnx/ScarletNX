/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* vk_cb_range / vk_sb_round: a whole checked step, at least one, within the device's uniform limit */
#include "nx/gpu/vk/vk.h"
#include <assert.h>
VkState g_vk;
int main(void) {
    g_vk.props.limits.maxUniformBufferRange = 65536;
    /* no robustness2: 16-byte uniform steps and byte-exact storage ranges, as before the feature */
    g_vk.robust_buffer2 = 0;
    g_vk.robust_ubo_align = 0;
    g_vk.robust_ssbo_align = 0;
    assert(vk_cb_range(0) == 16 && vk_cb_range(1) == 16 && vk_cb_range(16) == 16 && vk_cb_range(17) == 32);
    assert(vk_cb_range(70000) == 65536);
    assert(vk_sb_round(17) == 17);
    /* a 256-byte uniform step and a 4-byte storage step: an empty binding spans a whole step, ranges stay aligned */
    g_vk.robust_buffer2 = 1;
    g_vk.robust_ubo_align = 256;
    g_vk.robust_ssbo_align = 4;
    assert(vk_cb_range(0) == 256 && vk_cb_range(1) == 256 && vk_cb_range(256) == 256 && vk_cb_range(257) == 512);
    assert(vk_cb_range(65536) == 65536 && vk_cb_range(70000) == 65536); /* clamped, still a multiple of 256 */
    assert(vk_sb_round(16) == 16 && vk_sb_round(17) == 20);
    /* a limit smaller than a step keeps the limit: a valid range over an aligned one */
    g_vk.props.limits.maxUniformBufferRange = 100;
    assert(vk_cb_range(200) == 100);
    return 0;
}
