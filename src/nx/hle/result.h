/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Horizon results, the ones the runtime answers, and the helpers that return them. A result is a 32-bit value, 0 for
   success, otherwise a module in bits 0-8 and a description from bit 9. The helpers are the idiom of Atmosphère's R_
   macros, written anew in C99: each statement macro is one statement (do { } while (0)), safe as the body of an if or
   else, and evaluates each argument at most once. Self-contained, so strict C99 code can use it */
#ifndef SNX_RESULT_H
#define SNX_RESULT_H
#include <stddef.h>

#define RC(mod, desc) ((mod) | ((desc) << 9))
#define RC_OK         0

/* every result the runtime answers is a row of result_codes.inc: SNX_RC_<MODULE>_<NAME>, its module's number
   SNX_RESULT_MODULE_<MODULE> */
enum {
#define SNX_RESULT_MODULE(module, number) SNX_RESULT_MODULE_##module = (number),
#include "nx/hle/result_codes.inc"
};
enum {
#define SNX_RESULT(module, symbol, description, name, source)                                                          \
    SNX_RC_##module##_##symbol = RC(SNX_RESULT_MODULE_##module, description),
#include "nx/hle/result_codes.inc"
};

/* a row of result_codes.inc: the result, the public sources' name for it ("MembershipRequired") and the source that
   name is from ("Switchbrew") */
typedef struct {
    unsigned int code;
    const char* name;
    const char* source;
} SnxResultInfo;
/* rc's row, or NULL when result_codes.inc has none */
const SnxResultInfo* snx_result_info(unsigned int rc);
/* rc as the console shows it, 2000 + its module, then its description, and its name when it has a row: "2124-5113
   MembershipRequired", "2124-5114". Into out as snprintf writes, whose count it returns */
int snx_result_text(char* out, size_t size, unsigned int rc);

#define R_SUCCEEDED(rc) ((rc) == 0)
#define R_FAILED(rc)    ((rc) != 0)

/* return success */
#define R_SUCCEED()                                                                                                    \
    do {                                                                                                               \
        return RC_OK;                                                                                                  \
    } while (0)
/* return rc */
#define R_THROW(rc)                                                                                                    \
    do {                                                                                                               \
        return (rc);                                                                                                   \
    } while (0)
/* return rc when cond is false (rc is evaluated only then) */
#define R_UNLESS(cond, rc)                                                                                             \
    do {                                                                                                               \
        if (!(cond)) return (rc);                                                                                      \
    } while (0)
/* evaluate expr once and return its result when it failed. The temporary is 32 bits, as a result is, and converts
   back to the function's int or u32 */
#define R_TRY(expr)                                                                                                    \
    do {                                                                                                               \
        unsigned int snx_r_try_result_ = (unsigned int)(expr);                                                         \
        if (snx_r_try_result_ != 0) return snx_r_try_result_;                                                          \
    } while (0)
#endif
