/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_SPIRV_EMITTER_FIXTURE_H
#define SNX_SPIRV_EMITTER_FIXTURE_H
#include <stddef.h>
#include <stdint.h>

/* Compute shader: one invocation writes 40 + 2 to set 0, binding 0, word 0. */
uint32_t* snx_test_spirv_compute(size_t* words);
/* Minimal graphics stages for checking Vulkan's entry point and output rules. */
uint32_t* snx_test_spirv_graphics(int fragment, size_t* words);
/* Compute shader: words 0 to n - 1 of set 0, binding 0, as floats with their subnormals flushed
   (spv_emit_flush_subnormal, the float test with signed_zeros), into words n to 2n - 1 */
uint32_t* snx_test_spirv_flush(uint32_t n, int signed_zeros, size_t* words);
/* Compute shader: the red of the sampler2D at set 0, binding 1, read at (0.5, 0.5) of level 0, as its bits into set 0,
   binding 0, word 0 */
uint32_t* snx_test_spirv_sample(size_t* words);
#endif
