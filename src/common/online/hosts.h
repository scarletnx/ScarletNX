/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The console maker's own service domains, which an emulator that offers the community's services in their place
   resolves to no address: a game that names one of them is told the name does not resolve, as on a console that
   cannot reach it */
#ifndef SNX_ONLINE_HOSTS_H
#define SNX_ONLINE_HOSTS_H

#ifdef __cplusplus
extern "C" {
#endif

/* 1 when host is one of the domains or a name under one (letters compared without case, a final dot taken), else 0 */
int snx_online_maker_host(const char* host);

#ifdef __cplusplus
}
#endif
#endif
