/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The results the runtime answers (nx/hle/result_codes.inc) and their lookup (nx/hle/result.c), built as strict C99:
   each row once, in its module's place, named by the table's rule, and found by its code; what snx_result_text
   writes. Every failure names its row */
#include "nx/hle/result.c"
#include <ctype.h>
#include <string.h>

static int failures;

#define CHECK(expr)                                                                                                    \
    do {                                                                                                               \
        if (!(expr)) {                                                                                                 \
            fprintf(stderr, "result_codes.c:%d: check failed: %s\n", __LINE__, #expr);                                 \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

/* the table's rows in file order, the module rows among them, with what result.c does not keep */
typedef struct {
    int is_module;
    const char* module;
    unsigned int number; /* a module's number, a result's description */
    const char* symbol;
    const char* name;
    unsigned int code;
} Row;
static const Row rows[] = {
#define SNX_RESULT_MODULE(module, number) {1, #module, number, "", "", 0},
#define SNX_RESULT(module, symbol, description, name, source)                                                          \
    {0, #module, description, #symbol, #name, SNX_RC_##module##_##symbol},
#include "nx/hle/result_codes.inc"
};
#define ROW_COUNT (sizeof rows / sizeof rows[0])

static void row_failed(const Row* r, const char* why) {
    if (r->is_module)
        fprintf(stderr, "result_codes.inc: SNX_RESULT_MODULE(%s, %u): %s\n", r->module, r->number, why);
    else
        fprintf(stderr, "result_codes.inc: SNX_RESULT(%s, %s, %u, %s): %s\n", r->module, r->symbol, r->number, r->name,
                why);
    failures++;
}

/* name in capitals, a _ before each word: before a capital after a small letter or a digit, and before the last
   capital of a run when a small letter follows it (InvalidCmifHeaderSize, SslErrorUnkownCa) */
static void capitals(const char* name, char* out, size_t size) {
    size_t n = 0, i;
    for (i = 0; name[i] && n + 2 < size; i++) {
        int c = (unsigned char)name[i];
        if (i && isupper(c) &&
            (islower((unsigned char)name[i - 1]) || isdigit((unsigned char)name[i - 1]) ||
             (isupper((unsigned char)name[i - 1]) && islower((unsigned char)name[i + 1]))))
            out[n++] = '_';
        out[n++] = (char)toupper(c);
    }
    out[n] = 0;
}

static void test_rows(void) {
    const Row* module = NULL;
    unsigned int results = 0, previous_module = 0, previous_description = 0;
    size_t i, j;
    for (i = 0; i < ROW_COUNT; i++) {
        const Row* r = &rows[i];
        if (r->is_module) {
            if (r->number < 1 || r->number > 0x1ff) row_failed(r, "the module is not 1 to 511");
            if (r->number <= previous_module) row_failed(r, "the modules are not in the order of their numbers");
            if (module && !results) row_failed(module, "the module has no result");
            module = r;
            previous_module = r->number;
            previous_description = 0;
            results = 0;
            continue;
        }
        if (!module || strcmp(module->module, r->module)) row_failed(r, "the result is not under its module's row");
        if (r->number < 1 || r->number > 0x1fff) row_failed(r, "the description is not 1 to 8191");
        if (r->number <= previous_description) row_failed(r, "the results are not in the order of their descriptions");
        previous_description = r->number;
        results++;
        if (module && r->code != (module->number | r->number << 9))
            row_failed(r, "the code is not its module and description");
        {
            char want[128];
            capitals(r->name, want, sizeof want);
            if (strcmp(want, r->symbol)) row_failed(r, "NAME is not Name in capitals with a _ before each word");
            if (!strncmp(r->name, "Result", 6) && isupper((unsigned char)r->name[6]))
                row_failed(r, "Name keeps its \"Result\"");
        }
        {
            const SnxResultInfo* info = snx_result_info(r->code);
            if (!info || info->code != r->code || strcmp(info->name, r->name)) row_failed(r, "the lookup misses it");
        }
        for (j = 0; j < i; j++)
            if (!rows[j].is_module && rows[j].code == r->code) row_failed(r, "a second row of one result");
    }
    if (module && !results) row_failed(module, "the module has no result");
    CHECK(SNX_RESULT_COUNT == sizeof snx_results / sizeof snx_results[0]);
    for (i = 0, j = 0; i < ROW_COUNT; i++) j += !rows[i].is_module;
    CHECK(j == SNX_RESULT_COUNT);
    for (i = 0; i < SNX_RESULT_COUNT; i++) {
        const char* s = snx_results[i].source;
        CHECK(!strcmp(s, "Switchbrew") || !strcmp(s, "Ryubing ResultNames.cs") || !strcmp(s, "Ryubing ResultCode.cs") ||
              !strcmp(s, "Eden") || !strcmp(s, "ScarletNX"));
    }
}

/* codes as the sources write them, not through RC */
static void test_codes(void) {
    const SnxResultInfo* r;
    CHECK(SNX_RESULT_MODULE_ACCOUNT == 124 && SNX_RESULT_MODULE_SF_HIPC == 11);
    CHECK(SNX_RC_SVC_INVALID_HANDLE == 0xE401 && SNX_RC_SVC_SESSION_CLOSED == 0xF601);
    CHECK(SNX_RC_SVC_NOT_IMPLEMENTED == 0x4201 && SNX_RC_SVC_MESSAGE_TOO_LARGE == 0x20801);
    CHECK(SNX_RC_ACCOUNT_MEMBERSHIP_REQUIRED == 0x27F27C);       /* Switchbrew's value */
    CHECK(SNX_RC_FS_MULTI_COMMIT_FILE_SYSTEM_LIMIT == 0x346C02); /* ResultNames.cs's key */
    CHECK(SNX_RC_FS_MULTI_COMMIT_HAS_OVERLAPPING_TARGETS == 0x327E02);
    CHECK(SNX_RC_SSL_SSL_ALERT_CLOSE_NOTIFY == 0xBBA7B && SNX_RC_SSL_SSL_ALERT_BAD_CERTIFICATE == 0xBCA7B);
    r = snx_result_info(SNX_RC_ACCOUNT_MEMBERSHIP_REQUIRED);
    CHECK(r && !strcmp(r->name, "MembershipRequired") && !strcmp(r->source, "Switchbrew"));
    r = snx_result_info(SNX_RC_RO_INVALID_MEMORY_STATE);
    CHECK(r && !strcmp(r->name, "InvalidMemoryState") && !strcmp(r->source, "ScarletNX"));
    CHECK(!snx_result_info(RC_OK));
    CHECK(!snx_result_info(RC(124, 5114)));
    CHECK(!snx_result_info(0xFFFFFFFFu));
}

static void test_text(void) {
    char text[64];
    CHECK(snx_result_text(text, sizeof text, SNX_RC_ACCOUNT_MEMBERSHIP_REQUIRED) == 28);
    CHECK(!strcmp(text, "2124-5113 MembershipRequired"));
    CHECK(snx_result_text(text, sizeof text, SNX_RC_SVC_INVALID_HANDLE) == 23 &&
          !strcmp(text, "2001-0114 InvalidHandle"));
    CHECK(snx_result_text(text, sizeof text, RC(124, 5114)) == 9 && !strcmp(text, "2124-5114"));
    CHECK(snx_result_text(text, sizeof text, RC_OK) == 9 && !strcmp(text, "2000-0000"));
    /* cut as snprintf cuts, the whole length returned */
    CHECK(snx_result_text(text, 10, SNX_RC_ACCOUNT_MEMBERSHIP_REQUIRED) == 28 && !strcmp(text, "2124-5113"));
    CHECK(snx_result_text(NULL, 0, SNX_RC_ACCOUNT_MEMBERSHIP_REQUIRED) == 28);
}

int main(void) {
    test_rows();
    test_codes();
    test_text();
    if (failures) {
        fprintf(stderr, "result_codes: %d failed\n", failures);
        return 1;
    }
    printf("result_codes: %u results in %u modules, ok\n", (unsigned)SNX_RESULT_COUNT,
           (unsigned)(ROW_COUNT - SNX_RESULT_COUNT));
    return 0;
}
