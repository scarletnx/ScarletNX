/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_CTR_ARCHIVES_H
#define SNX_CTR_ARCHIVES_H
#include <stddef.h>
/* The system archives' open stand-ins, defined by the file that cmake/ctr_system_archives.cmake generates */
extern const unsigned char snx_ctr_mii_data[], snx_ctr_country_list[], snx_ctr_shared_font[], snx_ctr_bad_word_list[];
extern const size_t snx_ctr_mii_data_size, snx_ctr_country_list_size, snx_ctr_shared_font_size,
    snx_ctr_bad_word_list_size;
#endif
