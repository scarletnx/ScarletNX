/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "3ds/aot/ctr_module.h"
#include "3ds/memory/host.h"
#include "common/utilities/common.h"
#include <openssl/evp.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#endif
static unsigned calls;
static void put(unsigned char* p, uint32_t x) {
    for (unsigned i = 0; i < 4; ++i) p[i] = (unsigned char)(x >> (i * 8));
}
static void advance(SnxArmCPU* c) {
    ++calls;
    c->r[0] = 7;
    c->r[15] += c->cpsr & SNX_ARM_T ? 2 : 4;
    c->exit = 0;
}
static void stop(SnxArmCPU* c) {
    ++calls;
    c->r[15] += c->cpsr & SNX_ARM_T ? 2 : 4;
    c->exit = SNX_ARM_SVC;
    c->syndrome = 3;
}
static void restart(SnxArmCPU* c) {
    c->r[15] = 0x100000;
    c->cpsr = 0x10;
    c->exit = c->syndrome = 0;
    calls = 0;
}
int main(void) {
    char error[512];
    char* root = snx_temp_dir(".");
    assert(root);
    char* dir = snx_path_join(root, "exefs");
    assert(!snx_mkdirs(dir));
    char* ex_path = snx_path_join(root, "exheader.bin");
    char* code_path = snx_path_join(dir, ".code");
    unsigned char ex[0x800] = {0}, code[0x3000] = {0};
    put(ex + 0x10, 0x100000);
    put(ex + 0x14, 1);
    put(ex + 0x18, 8);
    put(ex + 0x1c, 0x4000);
    put(ex + 0x20, 0x200000);
    put(ex + 0x24, 1);
    put(ex + 0x28, 16);
    put(ex + 0x30, 0x300000);
    put(ex + 0x34, 1);
    put(ex + 0x38, 4);
    put(ex + 0x3c, 0x1000);
    put(code, 0xe3a0002a);
    put(code + 4, 0xef000003);
    memcpy(code + 0x1000, "READONLY", 8);
    memcpy(code + 0x2000, "DATA", 4);
    /* a refused extraction names the path: a folder that is not there, then one without its ExHeader */
    {
        char *nowhere = snx_path_join(root, "nowhere"), *said = snx_format("%s is not a folder", nowhere);
        SnxCtrImage none;
        assert(!snx_ctr_image_load(nowhere, &none, error, sizeof error) && !strcmp(error, said));
        free(said);
        said = snx_format("%s/exheader.bin is not a file of 2048 bytes", root);
        assert(!snx_ctr_image_load(root, &none, error, sizeof error) && !strcmp(error, said));
        free(said);
        free(nowhere);
    }
    assert(!snx_write_file(ex_path, ex, sizeof ex) && !snx_write_file(code_path, code, sizeof code));
    SnxCtrImage image, other;
    assert(snx_ctr_image_load(root, &image, error, sizeof error));
    assert(image.code_size == sizeof code && image.stack_size == 0x4000 && image.bss_size == 0x1000);
    /* Provider failure must reject and clear the partially loaded image, not
       publish an uninitialized/partial identity or fall back to custom SHA. */
    assert(EVP_set_default_properties(NULL, "provider=scarletnx_missing") == 1);
    assert(!snx_ctr_image_load(root, &other, error, sizeof error));
    assert(strstr(error, "OpenSSL") && !other.code && !other.code_size);
    for (unsigned i = 0; i < sizeof other.identity; ++i) assert(!other.identity[i]);
    assert(EVP_set_default_properties(NULL, "") == 1);
    assert(snx_ctr_image_load(root, &other, error, sizeof error));
    assert(!memcmp(image.identity, other.identity, 32));
    snx_ctr_image_free(&other);
    SnxArmBlock arm[] = {advance, stop}, thumb[] = {advance, stop, NULL, NULL};
    SnxCtrModule m = {.magic = SNX_CTR_MODULE_MAGIC,
                      .abi = SNX_CTR_MODULE_ABI,
                      .descriptor_size = sizeof m,
                      .cpu_size = sizeof(SnxArmCPU),
                      .checked_size = sizeof(SnxArmCheckedCPU),
                      .arm_abi = SNX_ARM_ABI,
                      .checked_abi = SNX_ARM_CHECKED_ABI,
                      .codegen_abi = SNX_CTR_MODULE_CODEGEN_ABI,
                      .profile = SNX_ARM_PROFILE_ARM11,
                      .memory_mode = snx_ctr_module_memory(),
                      .pointer_size = sizeof(void*),
                      .text_address = 0x100000,
                      .text_bytes = 8,
                      .arm_count = 2,
                      .thumb_count = 4,
                      .arm_blocks = arm,
                      .thumb_blocks = thumb};
    memcpy(m.image_identity, image.identity, 32);
    assert(snx_ctr_module_compatible(&m, &image));
    for (unsigned i = 0; i < 14; ++i) {
        SnxCtrModule bad = m;
        /* All fourteen consecutive u32 ABI/layout fields must be checked. */
        ((unsigned char*)&bad)[offsetof(SnxCtrModule, abi) + i * 4] ^= 1;
        assert(!snx_ctr_module_compatible(&bad, &image));
    }
    SnxCtrModule bad = m;
    bad.magic ^= 1;
    assert(!snx_ctr_module_compatible(&bad, &image));
    bad = m;
    bad.image_identity[31] ^= 1;
    assert(!snx_ctr_module_compatible(&bad, &image));
    bad = m;
    bad.arm_blocks = NULL;
    assert(!snx_ctr_module_compatible(&bad, &image));
    bad = m;
    bad.thumb_blocks = NULL;
    assert(!snx_ctr_module_compatible(&bad, &image));
    assert(!snx_ctr_module_compatible(NULL, &image) && !snx_ctr_module_compatible(&m, NULL));
    for (unsigned model = 0; model < 2; ++model) {
        SnxCtrProcess* p = snx_ctr_process_new(&image, (SnxCtrModel)model);
        assert(p);
        SnxCtrAddressSpace* s = snx_ctr_process_space(p);
        SnxArmCPU* c = snx_ctr_process_cpu(p);
        assert(c->r[15] == 0x100000 && c->r[13] == SNX_CTR_STACK_TOP && c->cpsr == 0x10);
        /* compiled code's flat view of the space, where the host has one */
        assert(c->mem == snx_ctr_space_base(s) && c->mem_size == (c->mem ? CTR_HOST_SPACE : 0));
        assert(c->tpidruro == SNX_CTR_INITIAL_TLS);
        unsigned permissions;
        assert(snx_ctr_query(s, 0x100000, NULL, &permissions) == SNX_CTR_MEM_OK && permissions == 5);
        assert(snx_ctr_query(s, 0x200000, NULL, &permissions) == SNX_CTR_MEM_OK && permissions == 1);
        assert(snx_ctr_query(s, 0x300000, NULL, &permissions) == SNX_CTR_MEM_OK && permissions == 3);
        unsigned char bytes[16];
        assert(snx_ctr_read(s, 0x300000, bytes, 4) == SNX_CTR_MEM_OK && !memcmp(bytes, "DATA", 4));
        assert(snx_ctr_read(s, 0x301000, bytes, sizeof bytes) == SNX_CTR_MEM_OK);
        for (unsigned i = 0; i < sizeof bytes; ++i) assert(!bytes[i]);
        assert(snx_ctr_read(s, SNX_CTR_STACK_TOP - 16, bytes, sizeof bytes) == SNX_CTR_MEM_OK);
        for (unsigned i = 0; i < sizeof bytes; ++i) assert(!bytes[i]);
        assert(snx_ctr_read(s, SNX_CTR_INITIAL_TLS, bytes, sizeof bytes) == SNX_CTR_MEM_OK);
        for (unsigned i = 0; i < sizeof bytes; ++i) assert(!bytes[i]);
        assert(snx_ctr_query(s, SNX_CTR_STACK_TOP - 0x5000, NULL, NULL) == SNX_CTR_MEM_UNMAPPED);
        assert(snx_ctr_write(s, 0x100000, bytes, 4) == SNX_CTR_MEM_PERMISSION);
        assert(snx_ctr_write(s, 0x200000, bytes, 4) == SNX_CTR_MEM_PERMISSION);
        assert(snx_ctr_fetch(s, 0x300000, bytes, 4) == SNX_CTR_MEM_PERMISSION);
        uint64_t executed;
        restart(c);
        assert(snx_ctr_dispatch(&m, p, 1, &executed) == SNX_CTR_RUN_BUDGET && executed == 1 && calls == 1);
        assert(c->r[15] == 0x100004);
        assert(snx_ctr_dispatch(&m, p, 1, &executed) == SNX_CTR_RUN_EXIT && executed == 1 && calls == 2);
        restart(c);
        c->cpsr |= SNX_ARM_T;
        assert(snx_ctr_dispatch(&m, p, 4, &executed) == SNX_CTR_RUN_EXIT && executed == 2 && calls == 2);
        restart(c);
        c->r[15] += 8;
        assert(snx_ctr_dispatch(&m, p, 4, &executed) == SNX_CTR_RUN_UNSUPPORTED && !executed && !calls);
        restart(c);
        c->cpsr |= SNX_ARM_T;
        c->r[15] += 4;
        assert(snx_ctr_dispatch(&m, p, 4, &executed) == SNX_CTR_RUN_UNSUPPORTED && !executed && !calls);
        restart(c);
        c->r[15] += 2;
        assert(snx_ctr_dispatch(&m, p, 4, &executed) == SNX_CTR_RUN_FAULT && c->exit == SNX_ARM_ALIGNMENT && !calls);
        restart(c);
        c->r[15] = 0x200000;
        assert(snx_ctr_dispatch(&m, p, 4, &executed) == SNX_CTR_RUN_FAULT && c->exit == SNX_ARM_PREFETCH_ABORT &&
               !calls);
        restart(c);
        assert(snx_ctr_protect(s, 0x100000, 4096, 1) == SNX_CTR_MEM_OK);
        assert(snx_ctr_dispatch(&m, p, 4, &executed) == SNX_CTR_RUN_FAULT && c->exit == SNX_ARM_PREFETCH_ABORT &&
               !calls);
        snx_ctr_process_free(p);
    }
    /* Editable manifests cannot supply permissions or identities. */
    char* manifest = snx_path_join(root, "manifest.ini");
    assert(!snx_write_file(manifest, "arbitrary invalid metadata", 26));
    assert(snx_ctr_image_load(root, &other, error, sizeof error));
    assert(!memcmp(image.identity, other.identity, 32));
    snx_ctr_image_free(&other);
    /* Real makerom homebrew can include BSS in .data's byte count. */
    put(ex + 0x38, 0x1004);
    assert(!snx_write_file(ex_path, ex, sizeof ex));
    assert(snx_ctr_image_load(root, &other, error, sizeof error));
    assert(other.segment[2].bytes == 0x1004 && other.code_size == image.code_size);
    SnxCtrProcess* makerom = snx_ctr_process_new(&other, SNX_CTR_OLD);
    assert(makerom);
    snx_ctr_process_free(makerom);
    snx_ctr_image_free(&other);
    put(ex + 0x38, 4);
    assert(!snx_write_file(ex_path, ex, sizeof ex));
    code[0] ^= 1;
    assert(!snx_write_file(code_path, code, sizeof code));
    assert(snx_ctr_image_load(root, &other, error, sizeof error));
    assert(!snx_ctr_module_compatible(&m, &other));
    snx_ctr_image_free(&other);
    code[0] ^= 1;
    assert(!snx_write_file(code_path, code, sizeof code - 1));
    assert(!snx_ctr_image_load(root, &other, error, sizeof error) && !other.code && !other.code_size);
    assert(!snx_write_file(code_path, code, sizeof code));
    FILE* append = snx_fopen(code_path, "ab");
    assert(append);
    assert(fputc(0, append) == 0);
    assert(!fclose(append));
    assert(!snx_ctr_image_load(root, &other, error, sizeof error));
    assert(!snx_write_file(code_path, code, sizeof code));
    put(ex + 0x20, 0x100000);
    assert(!snx_write_file(ex_path, ex, sizeof ex));
    assert(!snx_ctr_image_load(root, &other, error, sizeof error));
    put(ex + 0x20, 0x200000);
    put(ex + 0x3c, UINT32_MAX);
    assert(!snx_write_file(ex_path, ex, sizeof ex));
    assert(!snx_ctr_image_load(root, &other, error, sizeof error));
    put(ex + 0x3c, 0x1000);
    assert(!snx_write_file(ex_path, ex, sizeof ex));
    const uint32_t malformed[][2] = {{0x10, 0x100001}, {0x14, UINT32_MAX}, {0x18, 1},        {0x18, 4097},
                                     {0x1c, 0},        {0x1c, 0x800001},   {0x20, 0x100000}, {0x30, 0x3fff000},
                                     {0x38, 8193},     {0x3c, UINT32_MAX}};
    for (unsigned i = 0; i < sizeof malformed / sizeof *malformed; ++i) {
        unsigned char changed[sizeof ex];
        memcpy(changed, ex, sizeof ex);
        put(changed + malformed[i][0], malformed[i][1]);
        assert(!snx_write_file(ex_path, changed, sizeof changed));
        assert(!snx_ctr_image_load(root, &other, error, sizeof error) && !other.code);
    }
    /* the Old 3DS system mode is 0x20E's high four bits, beside the ideal processor and affinity mask in its low four;
       a mode no Old 3DS has is refused by its number */
    for (unsigned mode = 0; mode < 16; ++mode) {
        unsigned char changed[sizeof ex];
        memcpy(changed, ex, sizeof ex);
        changed[0x20e] = (unsigned char)(mode << 4 | 0xd);
        assert(!snx_write_file(ex_path, changed, sizeof changed));
        if (mode == 0 || (mode >= 2 && mode <= 5)) {
            assert(snx_ctr_image_load(root, &other, error, sizeof error) && other.system_mode == mode);
            snx_ctr_image_free(&other);
            continue;
        }
        char* said = snx_format("the ExHeader asks for system mode %u, which no Old 3DS has (0, 2, 3, 4 or 5)", mode);
        assert(!snx_ctr_image_load(root, &other, error, sizeof error) && !other.code && !strcmp(error, said));
        free(said);
    }
    assert(!snx_write_file(ex_path, ex, sizeof ex));
#ifndef _WIN32
    assert(!remove(code_path));
    assert(!symlink("../../missing", code_path));
    assert(!snx_ctr_image_load(root, &other, error, sizeof error));
#endif
    snx_ctr_image_free(&image);
    assert(!snx_remove_tree(root));
    free(manifest);
    free(code_path);
    free(ex_path);
    free(dir);
    free(root);
    puts("CTR image/process mappings, identity/ABI checks and bounded dispatcher passed");
    return 0;
}
