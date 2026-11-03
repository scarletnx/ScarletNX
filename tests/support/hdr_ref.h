/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The HDR passes' arithmetic on the CPU, from the published definitions (SMPTE ST 2084, the sRGB standard, ITU-R
   BT.2087), in double: what gpu/vk/hdr_compose.frag and hdr_encode.frag must write, so the GPU's output is held to a
   reference that shares no code with them. tests/support/hdr_ref.c */
#ifndef SNX_TEST_SUPPORT_HDR_REF_H
#define SNX_TEST_SUPPORT_HDR_REF_H

double hdr_ref_srgb_to_linear(double c);
double hdr_ref_linear_to_srgb(double c);
/* nits to the PQ code (0 to 1) and back */
double hdr_ref_pq_encode(double nits);
double hdr_ref_pq_decode(double code);
/* an SDR pixel's linear light to its nits: white at paper, the boost lifting only the highlights */
double hdr_ref_sdr_nits(double linear, double paper, double boost);
/* nits to the SDR range, paper white at 1 */
double hdr_ref_tonemap(double nits, double paper, double peak);
/* the nits the composition writes for an SDR sRGB code, and the SDR sRGB code the encode writes for nits */
double hdr_ref_sdr_code_nits(double code, double paper, double boost);
double hdr_ref_sdr_nits_code(double nits, double paper, double peak);
/* BT.709's primaries to BT.2020's, and back; and BT.2020's to Display P3's */
void hdr_ref_bt709_to_2020(const double in[3], double out[3]);
void hdr_ref_bt2020_to_709(const double in[3], double out[3]);
void hdr_ref_bt2020_to_p3(const double in[3], double out[3]);
/* an encoded sRGB code's contrast expanded by e: the same 0 and 1, more contrast between them */
double hdr_ref_expand_code(double code, double e);
/* a linear value through the same expansion (the encoded curve, then back): what the shader's colour scaling targets */
double hdr_ref_expand_linear(double lin, double e);
#endif
