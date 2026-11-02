/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Mozilla's root certificates as curl extracts them (curl.se/ca), compiled in by cmake/cacert.cmake: the roots the
   launcher's HTTPS trusts (https.c), the same on every system, as Eden carries them with its bundled OpenSSL */
#ifndef SNX_COMMON_CACERT_H
#define SNX_COMMON_CACERT_H
#include <stddef.h>

extern const unsigned char snx_cacert_pem[]; /* PEM, a NUL after it */
extern const size_t snx_cacert_pem_size;     /* without the NUL */
extern const char snx_cacert_date[];         /* the extract's date, YYYY-MM-DD */
#endif
