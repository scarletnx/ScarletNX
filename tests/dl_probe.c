/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A library no test links: platform_posix opens it to see plat_dl_loaded find a library only once it is loaded */
int snx_test_dl_probe(void);
int snx_test_dl_probe(void) { return 42; }
