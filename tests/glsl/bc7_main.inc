/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The BC7 encoder's body (src/nx/gpu/texture/bc7_encode.glsl) over RGBA8 texels, for the tests: no shader reads
   texels any more (astc_bc7.comp encodes each block from the ASTC blocks it decoded), so this is what gives the
   encoder a picture, and what the transcoder's blocks are held to: an invocation a 4x4 block of a w x h strip of rgba,
   64 a workgroup along a row of blocks, a block past the edge repeating its last texels, as astc_bc7.glsl does. Included in
   a struct after the body, which declares width, height, blocks_x, blocks_y, rgba and bc7_blocks */
void main() {
    const uint bx = gl_GlobalInvocationID.x, by = gl_GlobalInvocationID.y, z = gl_WorkGroupID.z;
    if (bx >= blocks_x || by >= blocks_y) return;
    for (uint k = 0u; k < 16u; k++) {
        const uint x = min(bx * 4u + (k & 3u), width - 1u), y = min(by * 4u + (k >> 2u), height - 1u);
        px[k] = rgba[(z * height + y) * width + x];
    }
    bc7_blocks[(z * blocks_y + by) * blocks_x + bx] = Encode();
}
