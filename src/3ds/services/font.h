/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_CTR_FONT_H
#define SNX_CTR_FONT_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* The shared font block of APT:GetSharedFont, in block (size bytes, which the caller zeroed): the status header, then
   the font from the system archive's level 3 image (cbf_std.bcfnt.lz) decompressed, its offsets made absolute for a
   block that lies at address. False when the archive has no such file or the font is malformed */
bool snx_ctr_shared_font_build(const unsigned char* archive, size_t archive_size, unsigned char* block, size_t size,
                               uint32_t address);
#endif
