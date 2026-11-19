/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Each WFC service's DNS server asked, over the internet, for the names a DS asks first: a service whose server no
   longer answers them fails here and leaves the list (src/wfc.c). Not a gate test: it needs the internet, and runs
   with SNX_TEST_ONLINE_LIVE=ON (ctest -L network) */
#include "common/online/dns.h"
#include "common/online/wfc.h"
#include <stdio.h>
#include <string.h>

int main(void) {
    static const char* const names[] = {"nas.nintendowifi.net", "conntest.nintendowifi.net"};
    int failed = 0;
    for (size_t i = 0; i < snx_online_wfc_preset_count(); ++i) {
        const SnxOnlineWfcPreset* p = snx_online_wfc_preset_at(i);
        for (size_t n = 0; n < sizeof names / sizeof *names; ++n) {
            uint8_t addr[4][4];
            char why[512] = "";
            int count = -1;
            for (int attempt = 0; attempt < 3 && count < 0; ++attempt) /* a datagram lost on the way */
                count = snx_online_dns_resolve(p->dns, 0, names[n], addr, 4, 3000, why, sizeof why);
            if (count > 0)
                printf("%-14s %u.%u.%u.%u  %-26s %u.%u.%u.%u\n", p->name, p->dns[0], p->dns[1], p->dns[2], p->dns[3],
                       names[n], addr[0][0], addr[0][1], addr[0][2], addr[0][3]);
            else {
                printf("%-14s %u.%u.%u.%u  %-26s %s\n", p->name, p->dns[0], p->dns[1], p->dns[2], p->dns[3], names[n],
                       count ? why : "no address");
                ++failed;
            }
        }
    }
    /* auto: every server at once, the fastest picked */
    uint8_t servers[16][4];
    int ms[16];
    char why[512] = "";
    size_t count = snx_online_wfc_preset_count();
    for (size_t i = 0; i < count; ++i) memcpy(servers[i], snx_online_wfc_preset_at(i)->dns, 4);
    int best = snx_online_wfc_fastest(servers, NULL, count, 2000, ms, why, sizeof why);
    for (size_t i = 0; i < count; ++i) printf("auto: %-14s %d ms\n", snx_online_wfc_preset_at(i)->name, ms[i]);
    if (best < 0) {
        printf("auto: %s\n", why);
        ++failed;
    } else
        printf("auto: picked %s, %d ms\n", snx_online_wfc_preset_at((size_t)best)->name, ms[best]);
    return failed ? 1 : 0;
}
