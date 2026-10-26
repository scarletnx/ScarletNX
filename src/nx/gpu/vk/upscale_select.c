/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/gpu/vk/upscale_select.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#define VENDOR_NVIDIA   0x10DEu
#define VENDOR_QUALCOMM 0x5143u

static int equal_ci(const char* a, const char* b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        ++a;
        ++b;
    }
    return !*a && !*b;
}

static const char* const keys[SNX_UPSCALE_COUNT] = {"auto", "off", "bilinear", "fsr1", "nis", "sgsr"};
static const char* const labels[SNX_UPSCALE_COUNT] = {"Auto",      "None",       "Bilinear",
                                                      "AMD FSR 1", "NVIDIA NIS", "Qualcomm SGSR"};

int snx_upscale_parse(const char* value) {
    static const char* const words[] = {"auto",       "off",  "none",         "bilinear",  "bilinear blit",
                                        "fsr1",       "fsr",  "fsr 1",        "amd fsr 1", "nis",
                                        "nvidia nis", "sgsr", "qualcomm sgsr"};
    static const signed char ids[] = {SNX_UPSCALE_AUTO,     SNX_UPSCALE_OFF,  SNX_UPSCALE_OFF,  SNX_UPSCALE_BILINEAR,
                                      SNX_UPSCALE_BILINEAR, SNX_UPSCALE_FSR1, SNX_UPSCALE_FSR1, SNX_UPSCALE_FSR1,
                                      SNX_UPSCALE_FSR1,     SNX_UPSCALE_NIS,  SNX_UPSCALE_NIS,  SNX_UPSCALE_SGSR,
                                      SNX_UPSCALE_SGSR};
    size_t i;
    if (!value) return SNX_UPSCALE_AUTO;
    while (*value == ' ' || *value == '\t') value++;
    for (i = 0; i < sizeof words / sizeof words[0]; i++)
        if (equal_ci(value, words[i])) return ids[i];
    return SNX_UPSCALE_AUTO;
}

const char* snx_upscale_key(int filter) {
    return filter >= 0 && filter < SNX_UPSCALE_COUNT ? keys[filter] : keys[SNX_UPSCALE_AUTO];
}
const char* snx_upscale_label(int filter) {
    return filter >= 0 && filter < SNX_UPSCALE_COUNT ? labels[filter] : labels[SNX_UPSCALE_AUTO];
}

int snx_upscale_known_phone(uint32_t vendor, uint32_t device, const char* name) {
    /* Qualcomm's Vulkan device IDs are the Adreno's, high half 0x07: 730 is 0x07030000, and each later one adds
       0x10000. A driver that reports another number is known by its name */
    if (vendor != VENDOR_QUALCOMM) return 0;
    if (device >= 0x07030000u && device <= 0x0708FFFFu) return 1;
    if (name) {
        const char* at = strstr(name, "Adreno");
        long n;
        if (!at) return 0;
        at = strchr(at, '7');
        if (!at) return 0;
        n = strtol(at, NULL, 10);
        return n >= 730 && n < 800;
    }
    return 0;
}

int snx_upscale_auto_pick(uint32_t vendor, uint32_t device, const char* name) {
    if (vendor == VENDOR_NVIDIA) return SNX_UPSCALE_NIS;
    if (snx_upscale_known_phone(vendor, device, name)) return SNX_UPSCALE_SGSR;
    return SNX_UPSCALE_FSR1;
}

int snx_upscale_ratio(int* state, uint32_t src_w, uint32_t src_h, uint32_t dst_w, uint32_t dst_h) {
    double x, y;
    if (!state || !src_w || !src_h || !dst_w || !dst_h) return 0;
    x = (double)dst_w / (double)src_w;
    y = (double)dst_h / (double)src_h;
    if (*state > 0) {
        if (x < 1.25 || y < 1.25) *state = -1;
    } else if (*state < 0) {
        if (x >= 1.35 && y >= 1.35) *state = 1;
    } else if (x >= 1.30 && y >= 1.30)
        *state = 1;
    return *state > 0;
}

int snx_upscale_uses_sharpness(int filter) {
    return filter == SNX_UPSCALE_AUTO || filter == SNX_UPSCALE_FSR1 || filter == SNX_UPSCALE_NIS ||
           filter == SNX_UPSCALE_SGSR;
}
