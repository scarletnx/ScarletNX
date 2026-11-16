// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// The shading rate image conversion's body (src/nx/gpu/vk/shading_rate.glsl) run on the CPU through
// tests/glsl/glsl.hpp: tests/vk_shading_rate.c compares what it writes with tests/support/shading_rate_ref.c.
#include <cstdint>
#include "glsl.hpp"

namespace shading_rate_shader {
using namespace glsl;
// what shading_rate.comp declares, under its names: the push constants, and the two images as byte arrays
struct Converter : Invocation {
    uint size_x, size_y, game_w, game_h, texel_w, texel_h, game_texel, finest;
    uint table[4];
    const uint8_t* game;
    uint8_t* rate;
#define GAME_INDEX(x, y)    uint(game[(y) * game_w + (x)])
#define RATE_STORE(x, y, v) (rate[(y) * size_x + (x)] = uint8_t(v))
#include "nx/gpu/vk/shading_rate.glsl"
#undef GAME_INDEX
#undef RATE_STORE
};
} // namespace shading_rate_shader

// push: the 12 words of VkVrsPush (src/nx/gpu/vk/shading_rate.h), in its order; out: size_x x size_y bytes
extern "C" void shading_rate_glsl_run(const uint32_t push[12], const uint8_t* game, uint8_t* out) {
    using namespace shading_rate_shader;
    // the dispatch shading_rate.c makes: 8x8 invocations a workgroup
    uvec3 groups((push[0] + 7u) / 8u, (push[1] + 7u) / 8u, 1u);
    dispatch<Converter>(groups, uvec3(8u, 8u, 1u), [&](Converter& s) {
        s.size_x = push[0], s.size_y = push[1], s.game_w = push[2], s.game_h = push[3];
        s.texel_w = push[4], s.texel_h = push[5], s.game_texel = push[6], s.finest = push[7];
        for (int i = 0; i < 4; i++) s.table[i] = push[8 + i];
        s.game = game;
        s.rate = out;
    });
}
