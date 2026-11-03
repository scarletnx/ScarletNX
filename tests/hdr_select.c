/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The HDR setting's names and the numbers the shaders run (gpu/vk/hdr_select.c), held to the published definitions by
   tests/support/hdr_ref.c: no game data and no GPU. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "nx/gpu/vk/hdr_select.h"
#include "support/hdr_ref.h"

#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "hdr_select:%d: %s\n", __LINE__, #x);                                                      \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (0)
#define NEAR(a, b, eps) (fabs((double)(a) - (double)(b)) <= (eps))

static int check_names(void) {
    CHECK(snx_hdr_parse("off") == SNX_HDR_OFF);
    CHECK(snx_hdr_parse("Off") == SNX_HDR_OFF);
    CHECK(snx_hdr_parse("none") == SNX_HDR_OFF);
    CHECK(snx_hdr_parse("auto") == SNX_HDR_AUTO);
    CHECK(snx_hdr_parse("Auto") == SNX_HDR_AUTO);
    CHECK(snx_hdr_parse("on") == SNX_HDR_ON);
    CHECK(snx_hdr_parse(" HDR ") == SNX_HDR_ON);
    CHECK(snx_hdr_parse("hdr10") == SNX_HDR_ON);
    CHECK(snx_hdr_parse("yes") == SNX_HDR_OFF);   /* what it doesn't know is off */
    CHECK(snx_hdr_parse(NULL) == SNX_HDR_OFF);
    CHECK(snx_hdr_parse("") == SNX_HDR_OFF);
    CHECK(!strcmp(snx_hdr_key(SNX_HDR_OFF), "off"));
    CHECK(!strcmp(snx_hdr_key(SNX_HDR_AUTO), "auto"));
    CHECK(!strcmp(snx_hdr_key(SNX_HDR_ON), "on"));
    CHECK(!strcmp(snx_hdr_label(SNX_HDR_OFF), "Off"));
    CHECK(!strcmp(snx_hdr_label(SNX_HDR_AUTO), "Auto"));
    CHECK(!strcmp(snx_hdr_label(SNX_HDR_ON), "On"));
    CHECK(!strcmp(snx_hdr_key(99), "off"));
    CHECK(!strcmp(snx_hdr_label(-1), "Off"));

    CHECK(snx_hdr_source_parse("auto") == SNX_HDR_SRC_AUTO);
    CHECK(snx_hdr_source_parse("sdr") == SNX_HDR_SRC_SDR);
    CHECK(snx_hdr_source_parse("SRGB") == SNX_HDR_SRC_SDR);
    CHECK(snx_hdr_source_parse("linear") == SNX_HDR_SRC_LINEAR);
    CHECK(snx_hdr_source_parse("pq") == SNX_HDR_SRC_PQ);
    CHECK(snx_hdr_source_parse("ST2084") == SNX_HDR_SRC_PQ);
    CHECK(snx_hdr_source_parse("nonsense") == SNX_HDR_SRC_AUTO);
    CHECK(!strcmp(snx_hdr_source_key(SNX_HDR_SRC_PQ), "pq"));
    CHECK(!strcmp(snx_hdr_source_label(SNX_HDR_SRC_LINEAR), "Linear"));
    CHECK(snx_hdr_source_value(SNX_HDR_SRC_AUTO) == SNX_HDR_SRC_SDR);

    CHECK(snx_hdr_colorspace_parse("auto") == SNX_HDR_CS_AUTO);
    CHECK(snx_hdr_colorspace_parse("p3") == SNX_HDR_CS_P3);
    CHECK(snx_hdr_colorspace_parse("Display P3") == SNX_HDR_CS_P3);
    CHECK(snx_hdr_colorspace_parse("scRGB") == SNX_HDR_CS_SCRGB);
    CHECK(snx_hdr_colorspace_parse("linear") == SNX_HDR_CS_SCRGB);
    CHECK(snx_hdr_colorspace_parse("hdr10") == SNX_HDR_CS_PQ);
    CHECK(snx_hdr_colorspace_parse("PQ") == SNX_HDR_CS_PQ);
    CHECK(snx_hdr_colorspace_parse("nonsense") == SNX_HDR_CS_AUTO);
    CHECK(!strcmp(snx_hdr_colorspace_key(SNX_HDR_CS_P3), "p3"));
    CHECK(!strcmp(snx_hdr_colorspace_key(SNX_HDR_CS_SCRGB), "scrgb"));
    CHECK(!strcmp(snx_hdr_colorspace_key(SNX_HDR_CS_PQ), "hdr10"));
    CHECK(!strcmp(snx_hdr_colorspace_label(SNX_HDR_CS_P3), "Display P3"));
    CHECK(!strcmp(snx_hdr_colorspace_label(SNX_HDR_CS_PQ), "HDR10 (PQ)"));
    return 0;
}

/* the two implementations of each number agree, and the known values of the standards hold */
static int check_numbers(void) {
    /* the sRGB standard's known points */
    CHECK(NEAR(snx_hdr_srgb_to_linear(0.5f), 0.21404114, 1e-6));
    CHECK(NEAR(snx_hdr_linear_to_srgb(0.21404114f), 0.5, 1e-6));
    CHECK(NEAR(snx_hdr_srgb_to_linear(0.04045f), 0.04045 / 12.92, 1e-6));
    CHECK(snx_hdr_srgb_to_linear(0.0f) == 0.0f);
    CHECK(snx_hdr_srgb_to_linear(1.0f) == 1.0f);
    CHECK(snx_hdr_linear_to_srgb(1.0f) == 1.0f);
    /* PQ: the code of 100, 1000 and 10000 nits, and the round trip */
    CHECK(NEAR(snx_hdr_pq_encode(100.0f), 0.5080784215, 1e-5));
    CHECK(NEAR(snx_hdr_pq_encode(1000.0f), 0.7518270962, 1e-5));
    CHECK(NEAR(snx_hdr_pq_encode(10000.0f), 1.0, 1e-6));
    CHECK(snx_hdr_pq_encode(0.0f) == 0.0f);
    CHECK(NEAR(snx_hdr_pq_decode(0.5080784215f), 100.0, 0.05));
    CHECK(NEAR(snx_hdr_pq_decode(0.7518270962f), 1000.0, 0.5));
    for (int n = 0; n <= 100; ++n) {
        float nits = (float)n * 100.0f;
        CHECK(NEAR(snx_hdr_pq_decode(snx_hdr_pq_encode(nits)), nits, nits < 1 ? 1e-3 : nits * 2e-4));
    }
    /* the whole range agrees with the reference, in both directions */
    for (int i = 0; i <= 400; ++i) {
        float c = (float)i / 400.0f;
        CHECK(NEAR(snx_hdr_srgb_to_linear(c), hdr_ref_srgb_to_linear(c), 2e-6));
        CHECK(NEAR(snx_hdr_linear_to_srgb(c), hdr_ref_linear_to_srgb(c), 2e-6));
        CHECK(NEAR(snx_hdr_pq_encode(c * 10000.0f), hdr_ref_pq_encode(c * 10000.0), 1e-5));
        CHECK(NEAR(snx_hdr_pq_decode(c), hdr_ref_pq_decode(c), hdr_ref_pq_decode(c) * 1e-3 + 1e-3));
    }
    /* the SDR boost: white at paper, and the fourth power only at the top */
    CHECK(NEAR(snx_hdr_sdr_nits(0.0f, 200.0f, 0.35f), 0.0, 1e-6));
    CHECK(NEAR(snx_hdr_sdr_nits(1.0f, 200.0f, 0.35f), 270.0, 1e-4));
    CHECK(NEAR(snx_hdr_sdr_nits(0.5f, 200.0f, 0.35f), hdr_ref_sdr_nits(0.5, 200.0, 0.35), 1e-4));
    CHECK(NEAR(snx_hdr_sdr_nits(0.5f, 200.0f, 0.0f), 100.0, 1e-4));
    CHECK(NEAR(snx_hdr_sdr_nits(1.0f, 200.0f, 0.0f), 200.0, 1e-4));
    /* the knee: the SDR range even up to the paper white, the headroom above it in the last 10%, the peak at 1 */
    CHECK(NEAR(snx_hdr_tonemap(100.0f, 200.0f, 1000.0f), 0.45, 1e-6));
    CHECK(NEAR(snx_hdr_tonemap(200.0f, 200.0f, 1000.0f), 0.9, 1e-6));
    CHECK(NEAR(snx_hdr_tonemap(1000.0f, 200.0f, 1000.0f), 1.0, 1e-6));
    CHECK(NEAR(snx_hdr_tonemap(1000.0f, 200.0f, 1000.0f), hdr_ref_tonemap(1000.0, 200.0, 1000.0), 1e-6));
    CHECK(snx_hdr_tonemap(0.0f, 200.0f, 1000.0f) == 0.0f);
    CHECK(snx_hdr_tonemap(5000.0f, 200.0f, 1000.0f) == 1.0f); /* above the peak clips at it */
    {
        float last = -1.0f;
        for (int i = 0; i <= 200; ++i) {
            float nits = (float)i * 40.0f; /* to 8000 nits */
            float y = snx_hdr_tonemap(nits, 200.0f, 1000.0f);
            CHECK(y >= last); /* monotonic */
            CHECK(y <= 1.0f);
            last = y;
        }
    }
    /* the SDR expansion: the ends stay, the darks go darker, the upper midtones brighter */
    CHECK(NEAR(snx_hdr_expand_code(0.0f, 1.0f), 0.0, 1e-7));
    CHECK(NEAR(snx_hdr_expand_code(1.0f, 1.0f), 1.0, 1e-7));
    CHECK(NEAR(snx_hdr_expand_code(0.5f, 1.0f), 0.5, 1e-7));
    CHECK(snx_hdr_expand_code(0.2f, 0.4f) < 0.2f);
    CHECK(snx_hdr_expand_code(0.8f, 0.4f) > 0.8f);
    for (int i = 1; i <= 100; ++i) CHECK(snx_hdr_expand_code(i / 100.0f, 1.0f) > snx_hdr_expand_code((i - 1) / 100.0f, 1.0f));
    for (int n = 0; n <= 200; ++n) {
        float c = (float)n / 200.0f;
        CHECK(NEAR(snx_hdr_expand_code(c, 0.4f), hdr_ref_expand_code(c, 0.4), 1e-6));
    }
    /* BT.709 -> BT.2020 -> BT.709, and the white point */
    {
        float rgb[3] = {1.0f, 1.0f, 1.0f}, back[3];
        double ref[3], ref_back[3];
        snx_hdr_bt709_to_bt2020(rgb);
        CHECK(NEAR(rgb[0], 1.0, 1e-4) && NEAR(rgb[1], 1.0, 1e-4) && NEAR(rgb[2], 1.0, 1e-4));
        back[0] = 0.63f;
        back[1] = 0.21f;
        back[2] = 0.05f;
        rgb[0] = back[0];
        rgb[1] = back[1];
        rgb[2] = back[2];
        ref[0] = back[0];
        ref[1] = back[1];
        ref[2] = back[2];
        snx_hdr_bt709_to_bt2020(rgb);
        hdr_ref_bt709_to_2020(ref, ref_back);
        CHECK(NEAR(rgb[0], ref_back[0], 1e-5) && NEAR(rgb[1], ref_back[1], 1e-5) && NEAR(rgb[2], ref_back[2], 1e-5));
        snx_hdr_bt2020_to_bt709(rgb);
        CHECK(NEAR(rgb[0], back[0], 2e-4) && NEAR(rgb[1], back[1], 2e-4) && NEAR(rgb[2], back[2], 2e-4));
    }
    /* BT.2020 -> Display P3: the white point stays, and a P3 red is back inside P3's primaries */
    {
        float rgb[3] = {1.0f, 1.0f, 1.0f};
        double ref[3], ref_back[3];
        snx_hdr_bt2020_to_p3(rgb);
        CHECK(NEAR(rgb[0], 1.0, 1e-4) && NEAR(rgb[1], 1.0, 1e-4) && NEAR(rgb[2], 1.0, 1e-4));
        rgb[0] = 0.63f;
        rgb[1] = 0.21f;
        rgb[2] = 0.05f;
        ref[0] = rgb[0];
        ref[1] = rgb[1];
        ref[2] = rgb[2];
        snx_hdr_bt2020_to_p3(rgb);
        hdr_ref_bt2020_to_p3(ref, ref_back);
        CHECK(NEAR(rgb[0], ref_back[0], 1e-5) && NEAR(rgb[1], ref_back[1], 1e-5) && NEAR(rgb[2], ref_back[2], 1e-5));
        /* BT.2020's own green is outside P3: in P3's coordinates it is beyond 1, which P3 cannot show */
        rgb[0] = 0.0f;
        rgb[1] = 1.0f;
        rgb[2] = 0.0f;
        snx_hdr_bt2020_to_p3(rgb);
        CHECK(rgb[1] > 1.0f && rgb[0] < 0.0f);
    }
    return 0;
}

/* the SDR range's share of the knee is even: two codes keep their ratio, so the SDR part of an HDR frame keeps its
   contrast when it is tone mapped down */
static int check_sdr_even(void) {
    double a = hdr_ref_tonemap(50.0, 200.0, 1000.0), b = hdr_ref_tonemap(100.0, 200.0, 1000.0);
    CHECK(fabs(a / b - 0.5) < 1e-9);
    return 0;
}

int main(void) {
    CHECK(!check_names());
    CHECK(!check_numbers());
    CHECK(!check_sdr_even());
    printf("PASS HDR names and numbers: off and on, the sources, PQ, sRGB and the knee against the standards\n");
    return 0;
}
