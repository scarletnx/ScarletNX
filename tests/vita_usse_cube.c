/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "vita/gpu/gxp.h"
#include "vita/gpu/usse.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    unsigned char gxp[216] = {0};
    UsseHints hints = {0};
    UsseShader shader;
    char error[256] = {0};

    snx_st_le32(gxp, GXP_MAGIC);
    snx_st_le32(gxp + GXP_FLAGS, GXP_FLAG_FRAGMENT);
    snx_st_le32(gxp + GXP_PARAMETER_COUNT, 1);
    snx_st_le32(gxp + GXP_PARAMETERS, 144); /* parameter record at 184 */
    snx_st_le32(gxp + GXP_PRIMARY_COUNT, 1);
    snx_st_le32(gxp + GXP_PRIMARY, 112); /* instruction at 176 */
    snx_st_le32(gxp + GXP_DEPENDENT_SAMPLER_COUNT, 1);
    snx_st_le32(gxp + GXP_DEPENDENT_SAMPLERS, 36); /* dependent sampler at 168 */
    snx_st_le32(gxp + GXP_CONTAINER_COUNT, 1);
    snx_st_le32(gxp + GXP_CONTAINERS, 12); /* container at 160 */

    snx_st_le16(gxp + 160, 19); /* sampler table's base SA container */
    snx_st_le16(gxp + 164, 1);  /* base SA register */
    snx_st_le16(gxp + 168, 0);  /* texture zero */
    snx_st_le16(gxp + 170, 0);  /* dependent sampler at the base SA register */
    snx_st_le64(gxp + 176, 0xe0000800c0000080ull); /* SMP, dim=3, sampler in SA register 1 */
    gxp[184 + 4] = GXP_CATEGORY_SAMPLER;
    gxp[184 + 7] = 0x10; /* cube sampler */
    snx_st_le32(gxp + 184 + 8, 1);
    snx_st_le32(gxp + 184 + 12, 0);
    hints.texture_formats[0] = 0;

    if (!usse_translate(gxp, sizeof gxp, &hints, false, &shader, error, sizeof error)) {
        fprintf(stderr, "cube SMP translation failed: %s\n", error);
        return 1;
    }
    assert(shader.fragment && shader.sampler_mask == 1 && shader.words && shader.count);
    usse_shader_free(&shader);
    return 0;
}
