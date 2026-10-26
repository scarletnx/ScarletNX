/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The upscaler setting's names, Auto's vendor choice and phone list, and the ratio Auto runs at (gpu/vk/upscale_select.c):
   the rules the presenter and the menu rely on, without a GPU. */
#include "nx/gpu/vk/upscale_select.h"
#include <assert.h>
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
    int state;

    CHECK(snx_upscale_parse("auto") == SNX_UPSCALE_AUTO);
    CHECK(snx_upscale_parse("off") == SNX_UPSCALE_OFF);
    CHECK(snx_upscale_parse("none") == SNX_UPSCALE_OFF);
    CHECK(snx_upscale_parse("bilinear") == SNX_UPSCALE_BILINEAR);
    CHECK(snx_upscale_parse("fsr1") == SNX_UPSCALE_FSR1);
    CHECK(snx_upscale_parse("AMD FSR 1") == SNX_UPSCALE_FSR1);
    CHECK(snx_upscale_parse("nis") == SNX_UPSCALE_NIS);
    CHECK(snx_upscale_parse("sgsr") == SNX_UPSCALE_SGSR);
    CHECK(snx_upscale_parse("nonsense") == SNX_UPSCALE_AUTO);
    CHECK(snx_upscale_parse(NULL) == SNX_UPSCALE_AUTO);
    CHECK(!strcmp(snx_upscale_key(SNX_UPSCALE_OFF), "off"));
    CHECK(!strcmp(snx_upscale_label(SNX_UPSCALE_OFF), "None"));
    CHECK(!strcmp(snx_upscale_label(SNX_UPSCALE_FSR1), "AMD FSR 1"));

    /* Auto: NVIDIA gets NIS, a known Adreno 730 and later gets SGSR, every other vendor gets FSR 1 */
    CHECK(snx_upscale_auto_pick(0x10DE, 0x2684, "NVIDIA GeForce RTX 4090") == SNX_UPSCALE_NIS);
    CHECK(snx_upscale_auto_pick(0x5143, 0x07030000, "Adreno (TM) 730") == SNX_UPSCALE_SGSR);
    CHECK(snx_upscale_auto_pick(0x5143, 0x07040000, "Adreno (TM) 740") == SNX_UPSCALE_SGSR);
    CHECK(snx_upscale_auto_pick(0x5143, 0, "Adreno (TM) 660") == SNX_UPSCALE_FSR1);
    CHECK(snx_upscale_auto_pick(0x5143, 0, "Adreno (TM) 740") == SNX_UPSCALE_SGSR);
    CHECK(snx_upscale_auto_pick(0x1002, 0x7590, "AMD Radeon RX 9060 XT") == SNX_UPSCALE_FSR1);
    CHECK(snx_upscale_auto_pick(0x8086, 0, "Intel Arc") == SNX_UPSCALE_FSR1);
    CHECK(snx_upscale_auto_pick(0x106B, 0, "Apple M2") == SNX_UPSCALE_FSR1);
    CHECK(snx_upscale_auto_pick(0x1234, 0, "Something else") == SNX_UPSCALE_FSR1);
    CHECK(snx_upscale_known_phone(0x5143, 0x07030000, NULL));
    CHECK(!snx_upscale_known_phone(0x1002, 0x07030000, "not Qualcomm"));

    /* Auto's ratio: on at 1.30, on to 1.25, off below it, and back only at 1.35 after it went off */
    state = 0;
    CHECK(!snx_upscale_ratio(&state, 1000, 1000, 1000, 1000));
    CHECK(state == 0);
    CHECK(!snx_upscale_ratio(&state, 1000, 1000, 1299, 1299));
    CHECK(state == 0);
    CHECK(snx_upscale_ratio(&state, 1000, 1000, 1300, 1300));
    CHECK(state == 1);
    CHECK(snx_upscale_ratio(&state, 1000, 1000, 1250, 1250));
    CHECK(state == 1);
    CHECK(!snx_upscale_ratio(&state, 1000, 1000, 1249, 1249));
    CHECK(state == -1);
    CHECK(!snx_upscale_ratio(&state, 1000, 1000, 1349, 1349));
    CHECK(state == -1);
    CHECK(snx_upscale_ratio(&state, 1000, 1000, 1350, 1350));
    CHECK(state == 1);
    /* one axis short is enough to stop it */
    state = 1;
    CHECK(!snx_upscale_ratio(&state, 1000, 1000, 2000, 1200));
    CHECK(state == -1);

    CHECK(snx_upscale_uses_sharpness(SNX_UPSCALE_AUTO));
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
