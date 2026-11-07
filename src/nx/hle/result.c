/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* From a result to its row of result_codes.inc, for the logs and the error applet to say the result by its name */
#include "nx/hle/result.h"
#include <stdio.h>

/* the sources a row's name comes from (result_codes.inc): an unknown one does not compile */
#define SNX_RESULT_SOURCE_SWITCHBREW   "Switchbrew"
#define SNX_RESULT_SOURCE_RESULT_NAMES "Ryubing ResultNames.cs"
#define SNX_RESULT_SOURCE_RYUBING      "Ryubing ResultCode.cs"
#define SNX_RESULT_SOURCE_EDEN         "Eden"
#define SNX_RESULT_SOURCE_NONE         "ScarletNX"

/* each row's place in the table */
enum {
#define SNX_RESULT(module, symbol, description, name, source) SNX_RESULT_AT_##module##_##symbol,
#include "nx/hle/result_codes.inc"
    SNX_RESULT_COUNT
};

static const SnxResultInfo snx_results[SNX_RESULT_COUNT] = {
#define SNX_RESULT(module, symbol, description, name, source)                                                          \
    {SNX_RC_##module##_##symbol, #name, SNX_RESULT_SOURCE_##source},
#include "nx/hle/result_codes.inc"
};

/* a switch: two rows of one result are two cases of one value, which the compiler refuses */
const SnxResultInfo* snx_result_info(unsigned int rc) {
    switch (rc) {
#define SNX_RESULT(module, symbol, description, name, source)                                                          \
    case SNX_RC_##module##_##symbol: return &snx_results[SNX_RESULT_AT_##module##_##symbol];
#include "nx/hle/result_codes.inc"
        default: return NULL;
    }
}

int snx_result_text(char* out, size_t size, unsigned int rc) {
    const SnxResultInfo* r = snx_result_info(rc);
    return snprintf(out, size, "%04u-%04u%s%s", 2000 + (rc & 0x1ff), (rc >> 9) & 0x1fff, r ? " " : "",
                    r ? r->name : "");
}
