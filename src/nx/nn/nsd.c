/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   The name's replacement is ported from Eden's src/core/hle/service/sockets/nsd.cpp (branch
   lizzie/refactor-networking-12), through the runtime's nsd:u service that this file replaces:
     SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project, GPL-3.0-or-later
     SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project, GPL-2.0-or-later */
/* nn::nsd, native (nsd.hook): the names of the console's servers in its network environment, with no nsd:u
   session. The behavior is that of a clean-room spec of the SDK's functions, never the SDK's code. The environment
   is the retail one, lp1. Any other nn::nsd function opens nsd:u itself, and sm stops the game with the name of
   this layer */
#include "nx/hle/hook/hook.h"
#include "hle/hook/hooks.hook.h"
#include <string.h>

#define NSD_ENVIRONMENT "lp1"   /* the retail network environment */
#define NSD_FQDN        0x100   /* nn::nsd::Fqdn: a string in 0x100 bytes */
#define NSD_IDENTIFIER  8       /* nn::nsd::EnvironmentIdentifier: a string in 8 bytes */
#define NSD_LP          1       /* nn::oe::ServerEnvironmentType of the retail servers (Dd is 0) */

/* in's name with each '%', the console's placeholder (accounts.%.ndas.srv.nintendo.net), replaced by the
   environment, as the console's nsd does (edenprobe: probe-%.edenprobe.invalid comes back
   probe-lp1.edenprobe.invalid). Every name resolves, a name the console does not know too, as it comes. A name with no
   end in its 0x100 bytes, or too long for them after the replacement, is cut there */
static u32 resolve(CPU* c, u64 out, u64 in) {
    const char* name = (const char*)hook_host(c, in);
    char fqdn[NSD_FQDN];
    size_t n = 0;
    for (size_t i = 0; i < NSD_FQDN && name[i] && n + 1 < sizeof fqdn; i++) {
        if (name[i] != '%') {
            fqdn[n++] = name[i];
            continue;
        }
        for (const char* e = NSD_ENVIRONMENT; *e && n + 1 < sizeof fqdn; e++) fqdn[n++] = *e;
    }
    memset(fqdn + n, 0, sizeof fqdn - n);
    memcpy(hook_host(c, out), fqdn, sizeof fqdn);
    return 0;
}
u32 nnnsd_resolve(CPU* c, u64 out, u64 in) { return resolve(c, out, in); }
/* the same: the SDK keeps only the command's own Result, never the service's inner one */
u32 nnnsd_resolve_ex(CPU* c, u64 out, u64 in) { return resolve(c, out, in); }
u32 nnnsd_server_environment_type(CPU* c) {
    (void)c;
    return NSD_LP;
}
/* the environment's name and zeros after it, as Eden answers the service's command */
void nnnsd_get_environment_identifier(CPU* c, u64 out) {
    char name[NSD_IDENTIFIER] = NSD_ENVIRONMENT;
    memcpy(hook_host(c, out), name, sizeof name);
}
