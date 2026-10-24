/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* kobj_id and kobj_at for a test whose kernel objects are its own (allocated by its stubs, not in g_hle's pools): an
   object gets an id the first time one is asked for, and kobj_at finds it again. A test includes it once */
#ifndef SNX_TEST_KOBJ_IDS_H
#define SNX_TEST_KOBJ_IDS_H
#include "nx/hle/kernel/kernel.h"
#include <stdio.h>
#include <stdlib.h>

static KObj* test_kobj_ids[8192];
static u32 test_nkobj_ids;
u32 kobj_id(const KObj* o) {
    u32 i;
    if (!o) return 0;
    for (i = 0; i < test_nkobj_ids; i++)
        if (test_kobj_ids[i] == o) return i + 1;
    if (test_nkobj_ids == sizeof test_kobj_ids / sizeof *test_kobj_ids) {
        fprintf(stderr, "kobj_ids.h: more kernel objects than the test's table holds\n");
        abort();
    }
    test_kobj_ids[test_nkobj_ids] = (KObj*)o;
    return ++test_nkobj_ids;
}
KObj* kobj_at(u32 id) { return id && id <= test_nkobj_ids ? test_kobj_ids[id - 1] : NULL; }

#endif
