/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* src/nx/gpu/nvn/nvn.h compiles as strict ISO C99, and its objects hold the fields docs/NVN_HLE.md records: the
   command buffer's, in both SDK layouts read there, end before its reserved size */
#include "nx/gpu/nvn/nvn.h"
#include <stdio.h>

/* the end of the last command buffer field docs/NVN_HLE.md records in each layout: the memory callback's userdata at
   +0x70 (8 bytes) in the newer, the reserve margin at +0x70 (a u32) in the older */
#define CMDBUF_FIELDS_END_NEW 0x78
#define CMDBUF_FIELDS_END_OLD 0x74

typedef char check_cmdbuf_new[sizeof(NVNcommandBuffer) >= CMDBUF_FIELDS_END_NEW ? 1 : -1];
typedef char check_cmdbuf_old[sizeof(NVNcommandBuffer) >= CMDBUF_FIELDS_END_OLD ? 1 : -1];

int main(void) {
    /* two of its function pointer types, which every NVN function has */
    PFNNVNCOMMANDBUFFERINITIALIZEPROC init = 0;
    PFNNVNCOMMANDBUFFERFINALIZEPROC fini = 0;
    (void)init;
    (void)fini;
    printf("nvn.h: NVNcommandBuffer is %u bytes, NVNtexture %u\n", (unsigned)sizeof(NVNcommandBuffer),
           (unsigned)sizeof(NVNtexture));
    return 0;
}
