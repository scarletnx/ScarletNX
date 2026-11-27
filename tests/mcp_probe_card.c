/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The card the MCP server's probe run writes the mode file to (mcp_probe_card, mcp_int.h): the one the NRO reads, as
   scripts/amberlith.py picks it. Amberlith, from 1.0.0, names sdmc:/amberlith and reads it; a build before 1.0.0 knows
   sdmc:/edenprobe alone */
#include "nx/runtime/driver/mcp_int.h"
#include <stdio.h>
#include <string.h>

static int failures;

static void expect(const char* what, const char* nro, size_t size, const char* card) {
    const char* got = mcp_probe_card((const unsigned char*)nro, size);
    if (strcmp(got, card)) {
        fprintf(stderr, "mcp_probe_card: %s: sdmc:/%s, not sdmc:/%s\n", what, got, card);
        failures++;
    }
}

int main(void) {
    static const char amberlith[] = "NRO0\0sdmc:/edenprobe/mode\0sdmc:/amberlith/mode\0machine=";
    static const char edenprobe[] = "NRO0\0sdmc:/edenprobe/mode\0machine=";
    static const char at_end[] = "NRO0\0sdmc:/amberlith";
    static const char cut[] = "NRO0\0sdmc:/amberlit";
    expect("Amberlith's NRO, which names both", amberlith, sizeof amberlith - 1, "amberlith");
    expect("a build before 1.0.0", edenprobe, sizeof edenprobe - 1, "edenprobe");
    expect("the name as the NRO's last bytes", at_end, sizeof at_end - 1, "amberlith");
    expect("the name cut by the NRO's end", cut, sizeof cut - 1, "edenprobe");
    expect("an empty NRO", "", 0, "edenprobe");
    puts(failures ? "mcp_probe_card: FAILED" : "mcp_probe_card: ok");
    return failures != 0;
}
