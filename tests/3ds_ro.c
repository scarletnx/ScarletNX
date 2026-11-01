/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* ldr:ro: a static module and a small library built by hand. Initialize maps the module, LoadCRO rebases the
   library, relocates it, copies its data to the game's buffer, links it, cuts it to its fix level and registers it,
   and UnloadCRO and Shutdown give the memory back */
#include "3ds/runtime/internal.h"
#include "3ds/ipc/ctr_ipc.h"
#include "common/utilities/endian.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define COMMAND  (SNX_CTR_INITIAL_TLS + 0x80)
#define HEAP     0x08000000u
#define IMAGE    0x00800000u /* where the module and the library go, in the process image region */
#define FIELD(n) (0x80 + (n) * 4)
static uint32_t word(SnxCtrProcess* p, unsigned index) {
    unsigned char b[4];
    assert(snx_ctr_read_ram(p->space, COMMAND + index * 4, b, 4) == SNX_CTR_MEM_OK);
    return snx_ld_le32(b);
}
/* a request of header and the normal words in args, then the process id descriptor; the reply's result */
static uint32_t send(SnxCtrProcess* p, uint32_t session, uint32_t header, const uint32_t* args, unsigned count) {
    unsigned char bytes[64] = {0};
    snx_st_le32(bytes, header);
    for (unsigned i = 0; i < count; ++i) snx_st_le32(bytes + 4 + i * 4, args[i]);
    snx_st_le32(bytes + 4 + count * 4, 0x20);
    assert(snx_ctr_write_ram(p->space, COMMAND, bytes, 12 + count * 4) == SNX_CTR_MEM_OK);
    p->cpu.state.cpu.r[0] = session;
    assert(snx_ctr_svc(p, 0x32) == SNX_CTR_SVC_RETURN && !p->cpu.state.cpu.r[0]);
    return word(p, 1);
}
static void put(SnxCtrProcess* p, uint32_t va, const void* bytes, size_t size) {
    assert(snx_ctr_write_ram(p->space, va, bytes, size) == SNX_CTR_MEM_OK);
}
static uint32_t get(SnxCtrProcess* p, uint32_t va) {
    unsigned char b[4];
    assert(snx_ctr_read_ram(p->space, va, b, 4) == SNX_CTR_MEM_OK);
    return snx_ld_le32(b);
}
static void set_word(unsigned char* file, unsigned offset, uint32_t value) { snx_st_le32(file + offset, value); }
static void set_field(unsigned char* file, unsigned field, uint32_t value) { set_word(file, FIELD(field), value); }
/* header words, numbered from the word at +0x80 */
enum {
    MAGIC = 0, NAME = 1, FILE_SIZE = 4, CODE_OFFSET = 12, CODE_SIZE = 13, DATA_OFFSET = 14, DATA_SIZE = 15,
    MODULE_NAME = 16, MODULE_NAME_SIZE = 17, SEGMENT_OFFSET = 18, SEGMENT_NUM = 19, EXPORT_NAMED_OFFSET = 20,
    EXPORT_NAMED_NUM = 21, EXPORT_INDEXED_OFFSET = 22, EXPORT_INDEXED_NUM = 23, EXPORT_STRINGS_OFFSET = 24,
    EXPORT_STRINGS_SIZE = 25, EXPORT_TREE_OFFSET = 26, EXPORT_TREE_NUM = 27, IMPORT_MODULE_OFFSET = 28,
    EXTERNAL_OFFSET = 30, IMPORT_NAMED_OFFSET = 32, IMPORT_INDEXED_OFFSET = 34, IMPORT_ANON_OFFSET = 36,
    IMPORT_STRINGS_OFFSET = 38, STATIC_ANON_OFFSET = 40, INTERNAL_OFFSET = 42, INTERNAL_NUM = 43, STATIC_OFFSET = 44
};
/* a module with no content: every table empty at the end of the header */
static void empty_file(unsigned char* file, uint32_t size) {
    set_field(file, MAGIC, 0x304f5243);
    for (unsigned field = CODE_OFFSET; field <= STATIC_OFFSET; field += 2) set_field(file, field, 0x138);
    set_field(file, FILE_SIZE, size);
}
int main(void) {
    unsigned char code[0x3000] = {0};
    SnxCtrImage image = {.segment = {{0x100000, 1, 16}, {0x200000, 1, 4}, {0x300000, 1, 4}},
                         .stack_size = 0x4000,
                         .main_priority = 0x30,
                         .bss_size = 0x1000,
                         .code = code,
                         .code_size = sizeof code};
    SnxCtrProcess* p = snx_ctr_process_new(&image, SNX_CTR_OLD);
    assert(p);
    uint32_t s = 0, out;
    assert(!snx_ctr_session_create(p, SNX_CTR_SERVICE_RO, &s));
    /* the game's buffers: the module, the library, the data and the bss, committed heap */
    assert(!snx_ctr_kernel_memory(p, 3, HEAP, 0, 0x5000, 3, &out));
    const uint32_t crs_buffer = HEAP, cro_buffer = HEAP + 0x1000, data_buffer = HEAP + 0x3000, bss_buffer = HEAP + 0x4000;
    const uint32_t crs = IMAGE, cro = IMAGE + 0x2000;

    /* the library: code, its name, the segments, one exported symbol, one internal relocation, 8 bytes of data */
    unsigned char file[0x2000] = {0};
    empty_file(file, sizeof file);
    set_field(file, CODE_OFFSET, 0x138);
    set_field(file, CODE_SIZE, 0x10);
    set_field(file, MODULE_NAME, 0x148);
    set_field(file, MODULE_NAME_SIZE, 8);
    memcpy(file + 0x148, "TestLib", 8);
    set_field(file, SEGMENT_OFFSET, 0x150);
    set_field(file, SEGMENT_NUM, 4);
    const uint32_t segments[4][3] = {{0x138, 0x10, 0}, {0x148, 8, 1}, {0x1ac, 8, 2}, {0, 0x10, 3}};
    for (unsigned i = 0; i < 4; ++i)
        for (unsigned j = 0; j < 3; ++j) set_word(file, 0x150 + i * 12 + j * 4, segments[i][j]);
    set_field(file, EXPORT_NAMED_OFFSET, 0x180);
    set_field(file, EXPORT_NAMED_NUM, 1);
    set_word(file, 0x180, 0x19c);     /* the name, in the export strings */
    set_word(file, 0x184, 4 << 4);    /* code + 4 */
    set_field(file, EXPORT_TREE_OFFSET, 0x188);
    set_field(file, EXPORT_TREE_NUM, 2);
    set_word(file, 0x188 + 2, 0x8001 | 0x8001u << 16); /* the root: both ways to the end node 1 */
    set_word(file, 0x190 + 4, 0);                      /* node 1: the symbol 0 */
    set_field(file, EXPORT_INDEXED_OFFSET, 0x198);
    set_field(file, EXPORT_INDEXED_NUM, 1);
    set_word(file, 0x198, 4 << 4);
    set_field(file, EXPORT_STRINGS_OFFSET, 0x19c);
    set_field(file, EXPORT_STRINGS_SIZE, 4);
    memcpy(file + 0x19c, "exp", 4);
    for (unsigned field = IMPORT_MODULE_OFFSET; field <= STATIC_ANON_OFFSET; field += 2) set_field(file, field, 0x1a0);
    set_field(file, INTERNAL_OFFSET, 0x1a0);
    set_field(file, INTERNAL_NUM, 1);
    set_word(file, 0x1a0, 0 << 4 | 2);          /* the first word of the data segment */
    set_word(file, 0x1a4, 2 | 0 << 8);          /* an absolute address of the code segment */
    set_word(file, 0x1a8, 4);                   /* plus 4 */
    set_field(file, STATIC_OFFSET, 0x1ac);
    set_field(file, DATA_OFFSET, 0x1ac);
    set_field(file, DATA_SIZE, 8);
    put(p, cro_buffer, file, sizeof file);
    unsigned char crs_file[0x1000] = {0};
    empty_file(crs_file, sizeof crs_file);
    put(p, crs_buffer, crs_file, sizeof crs_file);

    /* a library before Initialize */
    const uint32_t load[11] = {cro_buffer, cro, 0x2000, data_buffer, 0, 0x1000, bss_buffer, 0x1000, 1, 3, 0};
    assert(send(p, s, 0x000402c2, load, 11) == 0xd9612ff8u);
    const uint32_t init[3] = {crs_buffer, 0x1000, crs};
    assert(send(p, s, 0x000100c2, init, 3) == 0);
    assert(send(p, s, 0x000100c2, init, 3) == 0xd9612ff9u); /* Initialize twice */
    assert(snx_ctr_query(p->space, crs, NULL, NULL) == SNX_CTR_MEM_OK);

    /* a misaligned buffer, then the load: fix level 3 keeps the header, the code, the name and the segments */
    uint32_t bad[11];
    memcpy(bad, load, sizeof bad);
    bad[0] += 4;
    assert(send(p, s, 0x000402c2, bad, 11) == 0xd9012ff1u);
    assert(send(p, s, 0x000402c2, load, 11) == 0 && word(p, 2) == 0x1000);
    assert(snx_ctr_query(p->space, cro, NULL, NULL) == SNX_CTR_MEM_OK);
    assert(snx_ctr_query(p->space, cro + 0x1000, NULL, NULL) != SNX_CTR_MEM_OK); /* the cut tail is the game's again */
    /* rebased: the name and the first segment lie where the library does, and the data got its relocation */
    assert(get(p, cro + 0x80 + MODULE_NAME * 4) == cro + 0x148);
    assert(get(p, cro + 0x150) == cro + 0x138);
    assert(get(p, data_buffer) == cro + 0x138 + 4);
    assert(get(p, cro + 0x80 + 2 * 4) == 0 && get(p, crs + 0x80 + 2 * 4) == cro); /* registered after the module */
    /* the code is executable, the library read-only */
    unsigned permissions = 0;
    assert(snx_ctr_query(p->space, cro, NULL, &permissions) == SNX_CTR_MEM_OK && permissions == (SNX_CTR_READ | SNX_CTR_EXEC));
    assert(snx_ctr_write_ram(p->space, cro, "x", 1) == SNX_CTR_MEM_PERMISSION);

    /* the library once more at the same address is refused (mapped), and UnloadCRO frees it */
    assert(send(p, s, 0x000402c2, load, 11) != 0);
    const uint32_t unload[3] = {cro, 0, cro_buffer};
    assert(send(p, s, 0x000500c2, unload, 3) == 0);
    assert(snx_ctr_query(p->space, cro, NULL, NULL) != SNX_CTR_MEM_OK);
    assert(get(p, crs + 0x80 + 2 * 4) == 0);
    assert(send(p, s, 0x000500c2, unload, 3) == 0xd8a12c0du); /* not loaded: the header is the game's now */
    /* and it loads again from the file, the library the game read once more */
    put(p, cro_buffer, file, sizeof file);
    assert(send(p, s, 0x000402c2, load, 11) == 0);
    assert(send(p, s, 0x000500c2, unload, 3) == 0);
    const uint32_t shutdown[1] = {crs_buffer};
    assert(send(p, s, 0x00080042, shutdown, 1) == 0);
    assert(snx_ctr_query(p->space, crs, NULL, NULL) != SNX_CTR_MEM_OK);
    assert(send(p, s, 0x00080042, shutdown, 1) == 0xd9612ff8u);
    snx_ctr_process_free(p);
    puts("ok");
    return 0;
}
