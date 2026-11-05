/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/gpu/vk/advanced_blend.h"
#include <stdio.h>
#include <string.h>

static int checks, failures;
static void check(int yes, const char* what) {
    checks++;
    if (!yes && failures++ < 12) fprintf(stderr, "FAIL: %s\n", what);
}
int main(void) {
    const VkAdvancedBlendCaps full = {1, 1, 8, 1, 1, 1};
    for (u32 mode = 0; mode < 64; mode++)
        for (u32 overlap = 0; overlap < 4; overlap++)
            for (u32 premultiplied = 0; premultiplied < 2; premultiplied++) {
                u64 state = nvn_set_field(0, NVN_BLEND_ADV, mode);
                state = nvn_set_field(state, NVN_BLEND_ADV_OVL, overlap);
                state = nvn_set_field(state, NVN_BLEND_ADV_PMS, premultiplied);
                VkAdvancedBlend key;
                int valid = !mode || (mode >= 5 && mode <= 50 && overlap < 3);
                check(vk_advanced_blend_key(&key, state, &full, 1, 0) == valid, "native mode/parameter gate");
                if (mode && valid) {
                    check(key.operation == vk_advanced_blend_operation(mode), "native operation map");
                    check(key.src_premultiplied == premultiplied && key.overlap == overlap, "native parameters");
                    VkAdvancedBlend same;
                    check(vk_advanced_blend_key(&same, nvn_set_field(state, NVN_BLEND_ADV_NPM, 1), &full, 1, 0) &&
                              !memcmp(&key, &same, sizeof key),
                          "normalized hint is not destination premultiplication");
                    VkAdvancedBlendCaps subset = full;
                    subset.all_operations = 0;
                    check(vk_advanced_blend_key(&key, state, &subset, 1, 0) ==
                              vk_advanced_blend_required_operation(mode),
                          "all-operations property gate");
                    subset = full;
                    subset.coherent = 0;
                    check(!vk_advanced_blend_key(&key, state, &subset, 1, 0),
                          "noncoherent operations need primitive order");
                    check(vk_advanced_blend_key(&key, state, &subset, 1, 1),
                          "explicit primitive order admits noncoherent");
                    subset = full;
                    subset.nonpremultiplied_source = 0;
                    check(vk_advanced_blend_key(&key, state, &subset, 1, 0) == (int)premultiplied,
                          "nonpremultiplied source property gate");
                    subset = full;
                    subset.correlated_overlap = 0;
                    check(vk_advanced_blend_key(&key, state, &subset, 1, 0) == !overlap,
                          "correlated overlap property gate");
                    subset = full;
                    subset.available = 0;
                    check(!vk_advanced_blend_key(&key, state, &subset, 1, 0), "absent native extension rejects state");
                    check(!vk_advanced_blend_key(&key, state, &full, 0, 0) &&
                              !vk_advanced_blend_key(&key, state, &full, 9, 0),
                          "color attachment count limits");
                }
            }
    check(vk_advanced_blend_operation(NVN_BLEND_ADVANCED_MODE_PLUS) == VK_BLEND_OP_PLUS_EXT &&
              vk_advanced_blend_operation(NVN_BLEND_ADVANCED_MODE_MULTIPLY) == VK_BLEND_OP_MULTIPLY_EXT &&
              vk_advanced_blend_operation(NVN_BLEND_ADVANCED_MODE_MINUS) == VK_BLEND_OP_MINUS_EXT &&
              vk_advanced_blend_operation(NVN_BLEND_ADVANCED_MODE_EXCLUSION) == VK_BLEND_OP_EXCLUSION_EXT &&
              vk_advanced_blend_operation(NVN_BLEND_ADVANCED_MODE_HSL_HUE) == VK_BLEND_OP_HSL_HUE_EXT,
          "noncontiguous operation groups map independently");
    u64 seen = 0;
    for (u32 mode = 5; mode <= 50; mode++) {
        u32 offset = vk_advanced_blend_operation(mode) - VK_BLEND_OP_ZERO_EXT;
        check(offset < 46 && !(seen & (1ull << offset)), "each native operation occurs exactly once");
        if (offset < 46) seen |= 1ull << offset;
    }
    check(seen == (1ull << 46) - 1, "mapping covers all46 native operations");
    printf("vk advanced blend: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
