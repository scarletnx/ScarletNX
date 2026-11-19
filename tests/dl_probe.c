/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A library no test links: platform_posix opens it to see plat_dl_loaded find a library only once it is loaded, and
   plat_dl_resident map its every part, the 4 MB below that nothing reads among them, as a game's code not run yet */
int snx_test_dl_probe(void);
int snx_test_dl_probe(void) { return 42; }
extern const unsigned char snx_test_dl_probe_cold[4u << 20];
const unsigned char snx_test_dl_probe_cold[4u << 20] = {1};
