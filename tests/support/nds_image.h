/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A card image the NDS tests make: a header and an ARM9 program of the test's own words, with no game's data. */
#ifndef SNX_TEST_NDS_IMAGE_H
#define SNX_TEST_NDS_IMAGE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define NDS_TEST_ADDRESS 0x02000000u /* where the program is loaded, and its entry */
/* The image at path: count words of ARM9 code, loaded and entered at NDS_TEST_ADDRESS. False when it cannot be written */
bool nds_test_image(const char* path, const uint32_t* words, size_t count);
/* The program the CPU tests run (its assembly is in nds_image.c): it installs an interrupt handler, turns the vertical
   blank's interrupt on, leaves the results of CLZ, QADD, a BLX into Thumb, the BIOS's Div and a second Thumb call at
   0x02100000 on, then counts frames at 0x02100014 with VBlankIntrWait, its handler counting interrupts at 0x02100018 */
extern const uint32_t nds_test_program[];
extern const size_t nds_test_program_words;
#endif
