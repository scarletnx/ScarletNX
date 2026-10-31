/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Offline write-tracking kernel. Both range boundaries cross summary groups and larger ranges cross a leaf.
   Each run verifies touched pages and clean neighbors after the timed command's workload. No guest data. */
#include "nx/gpu/maxwell/write_pages.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    char* end;
    u64 pages = strtoull(argv[1], &end, 10);
    if (*end || !pages || pages > 8192) return 2;
    u64 repeats = strtoull(argv[2], &end, 10);
    if (*end || !repeats || repeats > 1000000000) return 2;
    static WritePages table;
    const u64 base = 0x600000000ull - 8 * 4096;
    write_pages_stamp(&table, base, pages * 4096, 1);
    for (u64 i = 1; i <= repeats; i++) write_pages_stamp(&table, base, pages * 4096, i);
    for (u64 i = 0; i < pages; i++) {
        if (!write_pages_since(&table, repeats - 1, base + i * 4096, 1)) return 1;
        if (write_pages_since(&table, repeats, base + i * 4096, 1)) return 1;
    }
    if (write_pages_since(&table, 0, base - 1, 1)) return 1;
    if (write_pages_since(&table, 0, base + pages * 4096, 1)) return 1;
    puts("verified");
    return 0;
}
