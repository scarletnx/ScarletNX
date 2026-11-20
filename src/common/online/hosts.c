/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/online/hosts.h"
#include <string.h>

/* the console maker's domains of ScarletNX's Switch list (src/nx/hle/services/network/abi/blocked_hosts.inc, after
   Eden's sfdnsres), and the 3DS's and the DS's service domains, which are among them */
static const char* const domains[] = {
    "nintendo.net",       "nintendowifi.net", "nintendo-europe.com", "nintendo.com.hk", "nintendo.com.au",
    "nintendo.co.kr",     "nintendo.co.uk",   "nintendo.co.jp",      "nintendo.co.nz",  "nintendo.co.za",
    "nintendo.com",       "nintendo.jp",      "nintendo.tw",         "nintendo.at",     "nintendo.be",
    "nintendo.dk",        "nintendo.de",      "nintendo.es",         "nintendo.fi",     "nintendo.fr",
    "nintendo.gr",        "nintendo.hu",      "nintendo.it",         "nintendo.nl",     "nintendo.no",
    "nintendo.pt",        "nintendo.ru",      "nintendo.ch",         "nintendo.se",     "nintendoswitch.com.cn",
    "nintendoswitch.com",
};

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c; }

int snx_online_maker_host(const char* host) {
    size_t length = host ? strlen(host) : 0;
    if (length && host[length - 1] == '.') --length;
    for (size_t d = 0; d < sizeof domains / sizeof *domains; ++d) {
        size_t n = strlen(domains[d]), i;
        if (length < n || (length > n && host[length - n - 1] != '.')) continue;
        for (i = 0; i < n && lower((unsigned char)host[length - n + i]) == domains[d][i]; ++i) {}
        if (i == n) return 1;
    }
    return 0;
}
