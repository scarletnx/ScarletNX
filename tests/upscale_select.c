/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The upscaler setting's names and values (gpu/vk/upscale_select.c): FSR 1 is the default, and the names the menu and
   the .ini use agree. */
#include "nx/gpu/vk/upscale_select.h"
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            fprintf(stderr, "upscale_select:%d: %s\n", __LINE__, #c);                                                  \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

int main(void) {
    CHECK(snx_upscale_parse("off") == SNX_UPSCALE_OFF);
    CHECK(snx_upscale_parse("none") == SNX_UPSCALE_OFF);
    CHECK(snx_upscale_parse("bilinear") == SNX_UPSCALE_BILINEAR);
    CHECK(snx_upscale_parse("fsr1") == SNX_UPSCALE_FSR1);
    CHECK(snx_upscale_parse("fsr") == SNX_UPSCALE_FSR1);
    CHECK(snx_upscale_parse("AMD FSR 1") == SNX_UPSCALE_FSR1);
    CHECK(snx_upscale_parse("nis") == SNX_UPSCALE_NIS);
    CHECK(snx_upscale_parse("nvidia nis") == SNX_UPSCALE_NIS);
    CHECK(snx_upscale_parse("sgsr") == SNX_UPSCALE_SGSR);
    CHECK(snx_upscale_parse("qualcomm sgsr") == SNX_UPSCALE_SGSR);
    CHECK(snx_upscale_parse(" NIS") == SNX_UPSCALE_NIS);
    CHECK(snx_upscale_parse("nis \t") == SNX_UPSCALE_NIS);
    /* FSR 1 is the default */
    CHECK(snx_upscale_parse("nonsense") == SNX_UPSCALE_FSR1);
    CHECK(snx_upscale_parse(NULL) == SNX_UPSCALE_FSR1);
    CHECK(!strcmp(snx_upscale_key(SNX_UPSCALE_OFF), "off"));
    CHECK(!strcmp(snx_upscale_key(SNX_UPSCALE_FSR1), "fsr1"));
    CHECK(!strcmp(snx_upscale_label(SNX_UPSCALE_OFF), "None"));
    CHECK(!strcmp(snx_upscale_label(SNX_UPSCALE_FSR1), "AMD FSR 1"));
    CHECK(!strcmp(snx_upscale_key(SNX_UPSCALE_COUNT), "fsr1")); /* an unknown filter is FSR 1 */

    CHECK(snx_upscale_uses_sharpness(SNX_UPSCALE_FSR1));
    CHECK(snx_upscale_uses_sharpness(SNX_UPSCALE_NIS));
    CHECK(snx_upscale_uses_sharpness(SNX_UPSCALE_SGSR));
    CHECK(!snx_upscale_uses_sharpness(SNX_UPSCALE_OFF));
    CHECK(!snx_upscale_uses_sharpness(SNX_UPSCALE_BILINEAR));

    if (failures) {
        fprintf(stderr, "%d upscale selection checks failed\n", failures);
        return 1;
    }
    puts("upscale selection: ok");
    return 0;
}
