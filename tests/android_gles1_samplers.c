/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* GLES 1.1's samplers (src/android/bridge/hle_gles.c, included whole for its static): a texture's two filters and two
   wraps of three name one of 36 samplers, each made once. Every pair gets a sampler of its own filters and wraps, the
   last four too, which shared the first one's while the table held 32. Vulkan's vkCreateSampler is stood in for by
   one that keeps what it was asked: no device is made */
#include "android/bridge/hle_gles.c"
#include <assert.h>
#include <stdio.h>

static struct {
    VkSamplerCreateInfo asked[64];
    unsigned count;
} g_t;

static VKAPI_ATTR VkResult VKAPI_CALL create_sampler(VkDevice device, const VkSamplerCreateInfo* info,
                                                     const VkAllocationCallbacks* allocator, VkSampler* out) {
    (void)device, (void)allocator;
    assert(g_t.count < 64);
    g_t.asked[g_t.count] = *info;
    *out = (VkSampler)(uintptr_t)++g_t.count; /* the count as a handle: which request made it */
    return VK_SUCCESS;
}

int main(void) {
    static const unsigned filters[2] = {GL_NEAREST_, GL_LINEAR_},
                          wraps[3] = {GL_CLAMP_TO_EDGE_, GL_REPEAT_, GL_MIRRORED_REPEAT_};
    static const VkSamplerAddressMode modes[3] = {VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_REPEAT,
                                                  VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT};
    V.vk.vkCreateSampler = create_sampler;
    for (unsigned pass = 0; pass < 2; ++pass)
        for (unsigned mag = 0; mag < 2; ++mag)
            for (unsigned min = 0; min < 2; ++min)
                for (unsigned s = 0; s < 3; ++s)
                    for (unsigned t = 0; t < 3; ++t) {
                        Texture texture = {
                            .mag = filters[mag], .min = filters[min], .wrap_s = wraps[s], .wrap_t = wraps[t]};
                        uintptr_t made = (uintptr_t)sampler(&texture);
                        assert(made >= 1 && made <= g_t.count);
                        const VkSamplerCreateInfo* info = &g_t.asked[made - 1];
                        assert(info->magFilter == (mag ? VK_FILTER_LINEAR : VK_FILTER_NEAREST));
                        assert(info->minFilter == (min ? VK_FILTER_LINEAR : VK_FILTER_NEAREST));
                        assert(info->addressModeU == modes[s] && info->addressModeV == modes[t]);
                    }
    assert(g_t.count == 36); /* one for each pair, made once */
    puts("android_gles1_samplers: ok");
    return 0;
}
