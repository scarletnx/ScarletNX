/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The 4 GiB policy's boundaries that need no 4 GiB of data: the CIA input's size check and the extraction budget's
   arithmetic. Sparse files stand in for the sizes, and the CIA's tables are valid at each size, so the limit is what
   decides. The policy's value is written out here, not taken from the header, so the test states the contract. */
#include "3ds/loader/internal.h"
#include "common/utilities/bits.h"
#include "common/utilities/endian.h"
#include "support/sparse_file.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#define POLICY_CAP 0x100000000ull /* the 4 GiB the policy is documented to allow */
#define TITLE      0x0004000000123400ull
enum { TICKET_AT = 0x2040, TICKET_SIZE = 0x3c0, TMD_AT = 0x2400, TMD_SIZE = 0xc34 };

static void sha256(const void* data, size_t size, unsigned char out[32]) {
    assert(EVP_Digest(data, size, out, NULL, EVP_sha256(), NULL) == 1);
}
static bool write_at(FILE* f, uint64_t at, const void* data, size_t size) {
    return !snx_seek(f, at) && fwrite(data, 1, size, f) == size;
}
/* A CIA whose one content starts after its sections and runs to total: every table the loader checks is consistent,
   the content itself is a hole because ctr_cia_load only checks its span. */
static bool make_cia(const char* path, uint64_t total) {
    unsigned char header[0x2020] = {0}, ticket[TICKET_SIZE] = {0}, tmd[TMD_SIZE] = {0};
    const uint64_t content_at = snx_align_up(TMD_AT + TMD_SIZE, 64);
    assert(total > content_at);
    const uint64_t content = total - content_at;
    snx_st_le32(header, 0x2020);
    snx_st_le32(header + 0x0c, TICKET_SIZE);
    snx_st_le32(header + 0x10, TMD_SIZE);
    snx_st_le64(header + 0x18, content);
    header[0x20] = 0x80;
    snx_st_be32(ticket, 0x10000);
    snx_st_be64(ticket + 0x240 + 0x9c, TITLE);
    snx_st_be32(ticket + 0x240 + 0x164 + 4, 8);
    snx_st_be32(tmd, 0x10000);
    unsigned char* body = tmd + 0x240;
    snx_st_be64(body + 0x4c, TITLE);
    snx_st_be16(body + 0x9e, 1);
    unsigned char* entry = body + 0x9c4;
    snx_st_be32(entry, 1);
    snx_st_be16(entry + 4, 0);
    snx_st_be64(entry + 8, content);
    unsigned char* info = body + 0xc4;
    snx_st_be16(info + 2, 1);
    sha256(entry, 0x30, info + 4);
    sha256(info, 64 * 0x24, body + 0xa4);
    FILE* f = snx_fopen(path, "w+b");
    assert(f);
    bool supported = test_sparse_resize(f, total);
    if (supported) {
        assert(write_at(f, 0, header, sizeof header));
        assert(write_at(f, TICKET_AT, ticket, sizeof ticket));
        assert(write_at(f, TMD_AT, tmd, sizeof tmd));
    }
    assert(!fclose(f));
    return supported;
}
static SnxCtrExtractResult load_cia(const char* path, uint64_t* title, char* error, size_t error_size) {
    CtrReader r = {.error = error, .error_size = error_size, .quick = true};
    CtrSource s = {0};
    if (error_size) error[0] = 0;
    if (!(r.input = snx_fopen(path, "rb"))) return SNX_CTR_EXTRACT_IO;
    r.size = snx_file_size(path);
    bool ok = ctr_cia_load(&r, &s);
    if (ok && title) *title = s.title_id;
    ctr_cia_free(&s);
    fclose(r.input);
    return r.result;
}
static void budget_boundaries(const char* root) {
    char error[256];
    CtrReader r = {.error = error, .error_size = sizeof error};
    char* path = snx_path_join(root, "budget.bin");
    assert(path);
    /* one byte past 1 GiB is within the budget */
    r.output_bytes = 1ull << 30;
    assert(ctr_write(&r, path, "a", 1) && r.output_bytes == (1ull << 30) + 1);
    assert(!snx_remove_tree(path));
    /* exactly the new cap: a write that ends on it fits, one byte more is refused before any output */
    r.output_bytes = POLICY_CAP - 4;
    assert(ctr_write(&r, path, "abcd", 4) && r.output_bytes == POLICY_CAP);
    assert(!snx_remove_tree(path));
    r.output_bytes = POLICY_CAP;
    r.result = SNX_CTR_EXTRACT_OK;
    assert(!ctr_write(&r, path, "x", 1) && r.result == SNX_CTR_EXTRACT_LIMIT && !snx_path_exists(path));
    /* the budget is the accumulation, not one write's size */
    r.output_bytes = POLICY_CAP - 3;
    r.result = SNX_CTR_EXTRACT_OK;
    assert(ctr_write(&r, path, "ab", 2));
    assert(!snx_remove_tree(path));
    assert(!ctr_write(&r, path, "cd", 2) && r.result == SNX_CTR_EXTRACT_LIMIT && !snx_path_exists(path));
    /* a copy refused by the budget creates nothing either */
    r.output_bytes = POLICY_CAP;
    r.result = SNX_CTR_EXTRACT_OK;
    assert(!ctr_copy(&r, (CtrSpan){0, 1}, path, NULL) && r.result == SNX_CTR_EXTRACT_LIMIT && !snx_path_exists(path));
    free(path);
}
static int run(const char* root) {
    char error[256];
    uint64_t title = 0;
    char* input = snx_path_join(root, "input.cia");
    assert(input);
    if (!make_cia(input, POLICY_CAP + 1)) {
        puts("sparse files are not supported here");
        free(input);
        return 77;
    }
    /* one byte past the cap is refused with a reason */
    assert(load_cia(input, NULL, error, sizeof error) == SNX_CTR_EXTRACT_LIMIT && strstr(error, "4 GiB"));
    /* one byte past 1 GiB, and exactly the cap, are accepted */
    assert(make_cia(input, (1ull << 30) + 1));
    assert(load_cia(input, &title, error, sizeof error) == SNX_CTR_EXTRACT_OK && title == TITLE);
    assert(make_cia(input, POLICY_CAP));
    assert(load_cia(input, &title, error, sizeof error) == SNX_CTR_EXTRACT_OK && title == TITLE);
    budget_boundaries(root);
    free(input);
    puts("CIA input takes 4 GiB and the extraction budget holds at its boundaries");
    return 0;
}
int main(void) {
    char* root = snx_temp_dir(NULL);
    assert(root);
    int status = run(root);
    assert(!snx_remove_tree(root));
    free(root);
    return status;
}
