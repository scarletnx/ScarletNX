/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/gpu/vk/upscale_select.h"
#include "common/utilities/ascii.h"
#include <string.h>

static int equal_ci(const char* a, const char* b) {
    while (*a && *b) {
        if (snx_to_lower((unsigned char)*a) != snx_to_lower((unsigned char)*b)) return 0;
        ++a;
        ++b;
    }
    return !*a && !*b;
}

static const char* const keys[SNX_UPSCALE_COUNT] = {"off", "bilinear", "fsr1", "nis", "sgsr"};
static const char* const labels[SNX_UPSCALE_COUNT] = {"None", "Bilinear", "AMD FSR 1", "NVIDIA NIS", "Qualcomm SGSR"};

int snx_upscale_parse(const char* value) {
    static const char* const words[] = {"off",   "none",      "bilinear", "bilinear blit", "fsr1", "fsr",
                                        "fsr 1", "amd fsr 1", "nis",      "nvidia nis",    "sgsr", "qualcomm sgsr"};
    static const signed char ids[] = {SNX_UPSCALE_OFF,  SNX_UPSCALE_OFF,  SNX_UPSCALE_BILINEAR, SNX_UPSCALE_BILINEAR,
                                      SNX_UPSCALE_FSR1, SNX_UPSCALE_FSR1, SNX_UPSCALE_FSR1,     SNX_UPSCALE_FSR1,
                                      SNX_UPSCALE_NIS,  SNX_UPSCALE_NIS,  SNX_UPSCALE_SGSR,     SNX_UPSCALE_SGSR};
    char text[16];
    size_t i, n;
    if (!value) return SNX_UPSCALE_FSR1;
    while (snx_is_blank((unsigned char)*value)) value++;
    n = strlen(value);
    while (n && snx_is_blank((unsigned char)value[n - 1])) n--;
    if (n >= sizeof text) return SNX_UPSCALE_FSR1;
    memcpy(text, value, n);
    text[n] = 0;
    for (i = 0; i < sizeof words / sizeof words[0]; i++)
        if (equal_ci(text, words[i])) return ids[i];
    return SNX_UPSCALE_FSR1;
}

const char* snx_upscale_key(int filter) {
    return filter >= 0 && filter < SNX_UPSCALE_COUNT ? keys[filter] : keys[SNX_UPSCALE_FSR1];
}
const char* snx_upscale_label(int filter) {
    return filter >= 0 && filter < SNX_UPSCALE_COUNT ? labels[filter] : labels[SNX_UPSCALE_FSR1];
}

int snx_upscale_uses_sharpness(int filter) {
    return filter == SNX_UPSCALE_FSR1 || filter == SNX_UPSCALE_NIS || filter == SNX_UPSCALE_SGSR;
}
