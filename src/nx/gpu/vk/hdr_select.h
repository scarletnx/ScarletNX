/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The HDR setting's names, the frame's source colour space, and the tone mapping's numbers. Pure C, so the tests hold
   them to the names and to the arithmetic the shaders run. */
#ifndef SNX_GPU_VK_HDR_SELECT_H
#define SNX_GPU_VK_HDR_SELECT_H

/* hdr: Off shows the frame as it is. Auto turns it on only where the display takes an HDR image and reports headroom
   over its white (so an SDR-only display pays nothing). On always runs the passes, and the SDR range stands in where
   the display has no HDR. A frame that already holds HDR data is never lifted again */
enum { SNX_HDR_OFF, SNX_HDR_AUTO, SNX_HDR_ON, SNX_HDR_COUNT };

/* hdr_source: what the presented frame's pixels are. Auto reads it from the frame's format (a float target is linear
   HDR); the others name it for a game whose format doesn't say */
enum {
    SNX_HDR_SRC_AUTO,   /* read the format */
    SNX_HDR_SRC_SDR,    /* sRGB-encoded, 0 to 1: the console's display and nearly every frame */
    SNX_HDR_SRC_LINEAR, /* linear light, 1.0 = paper white, above it the frame's own HDR headroom */
    SNX_HDR_SRC_PQ,     /* PQ (SMPTE ST 2084), BT.2020: an HDR10 signal already encoded */
    SNX_HDR_SRC_COUNT
};

/* the .ini's value (off, on), off for one it doesn't know */
int snx_hdr_parse(const char* value);
/* the .ini's value, and the menu's label */
const char* snx_hdr_key(int mode);
const char* snx_hdr_label(int mode);

/* hdr_colorspace: the image an HDR display is given. Auto takes Display P3's wide gamut where the surface offers it
   (its white is the display's own), then scRGB, else HDR10 */
enum { SNX_HDR_CS_AUTO, SNX_HDR_CS_P3, SNX_HDR_CS_SCRGB, SNX_HDR_CS_PQ, SNX_HDR_CS_COUNT };

/* the .ini's value (auto, sdr, linear, pq), auto for one it doesn't know */
int snx_hdr_source_parse(const char* value);
const char* snx_hdr_source_key(int source);
const char* snx_hdr_source_label(int source);
/* the source named for the shader's uniform: auto is SDR, what the console's display shows */
int snx_hdr_source_value(int source);

/* the .ini's value (auto, p3, scrgb, hdr10), auto for one it doesn't know, and its key and label */
int snx_hdr_colorspace_parse(const char* value);
const char* snx_hdr_colorspace_key(int cs);
const char* snx_hdr_colorspace_label(int cs);

/* ---- the tone mapping's numbers, which hdr.c sends as the shader's push constants and tests/support/hdr_ref.c checks
   ---- */
/* SDR code (0 to 1, sRGB encoded) to linear light: the sRGB EOTF */
float snx_hdr_srgb_to_linear(float c);
/* linear light (0 to 1) to the sRGB encoded code */
float snx_hdr_linear_to_srgb(float c);
/* nits to the PQ code (0 to 1) and back: SMPTE ST 2084 with BT.2020's 10000 nits */
float snx_hdr_pq_encode(float nits);
float snx_hdr_pq_decode(float code);
/* An SDR pixel's linear value to its nits: white maps to paper_nits, and a share of the headroom lifts the highlights
   only (boost 0 to 1: nits = paper * (c + boost * c^4)) */
float snx_hdr_sdr_nits(float linear, float paper_nits, float boost);
/* HDR nits to SDR [0, 1], paper white at 1: values to paper_nits pass as they are, above it a knee compresses up to
   peak_nits (which stays under 1), so no SDR content moves */
float snx_hdr_tonemap(float nits, float paper_nits, float peak_nits);
/* BT.709's primaries to BT.2020's, and back, for the linear light of D65 white; and BT.2020's to Display P3's */
void snx_hdr_bt709_to_bt2020(float rgb[3]);
void snx_hdr_bt2020_to_bt709(float rgb[3]);
void snx_hdr_bt2020_to_p3(float rgb[3]);
/* the SDR range mapped into HDR: an encoded sRGB code's contrast expanded by e (0: the code, 1: a smoothstep), its
   black deeper and its upper midtones brighter, white unchanged */
float snx_hdr_expand_code(float code, float e);
#endif
