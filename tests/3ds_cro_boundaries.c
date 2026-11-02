/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "support/3ds_cro.h"

/* the addend of the last relocation of each batch, and the largest image that a case maps */
#define LAST_ADDEND 0x123u
#define IMAGE_LIMIT 0x400000u

/* A CRS (crs) or a module that imports from it, whose tables have large counts but still lie inside a small mapped
   image. The one name is as long as the scenario says, and is the export, or the import, or the module's name when
   by_module. image_size and batch_offset return the size of the image and where its external relocations start. */
static unsigned char* boundary_image(bool crs, const char* name, uint32_t exports, uint32_t relocations, bool by_module,
                                     uint32_t* image_size, uint32_t* batch_offset) {
    bool module_exports = by_module && crs;
    bool module_imports = by_module && !crs;
    uint32_t length = (uint32_t)strlen(name) + 1;
    uint32_t module_length = module_exports ? length : 8;
    uint32_t segments = (IMG_RODATA + module_length + 3) & ~3u;
    uint32_t named = segments + 4 * SEGMENT_RECORD;
    uint32_t indexed = named + exports * EXPORT_RECORD;
    uint32_t strings = indexed + (module_exports ? 4 : 0);
    uint32_t imports = (strings + length + 6 + 3) & ~3u;
    uint32_t external = imports + (module_imports ? IMPORT_MODULE_RECORD : 0);
    uint32_t import_named = external + relocations * RELOCATION_RECORD;
    uint32_t import_indexed = import_named + (!crs && !by_module ? IMPORT_RECORD : 0);
    uint32_t import_strings = import_indexed + (module_imports ? IMPORT_RECORD : 0);
    uint32_t tail = (import_strings + length + 3) & ~3u;
    uint32_t data = (tail + 4095) & ~4095u;
    *image_size = data + 4096;
    *batch_offset = external;
    unsigned char* b = calloc(1, *image_size);
    assert(b);
    cro_set(b, CRO_HEADER, CRO_MAGIC);
    cro_set(b, CRO_FILE_SIZE, *image_size);
    cro_set(b, CRO_BSS_SIZE, 0x100);
    cro_set(b, CRO_ON_UNRESOLVED, CRO_TAG(0, CRO_UNRESOLVED));
    cro_set(b, CRO_CODE_OFFSET, IMG_CODE);
    cro_set(b, CRO_CODE_SIZE, 0x1000);
    cro_set(b, CRO_DATA_OFFSET, data);
    cro_set(b, CRO_DATA_SIZE, 0x100);
    cro_set(b, CRO_MODULE_NAME, IMG_NAME);
    cro_set(b, CRO_MODULE_NAME_SIZE, module_length);
    memcpy(b + IMG_NAME, module_exports ? name : "module", module_exports ? length : 7);
    cro_set(b, CRO_SEGMENTS, segments);
    cro_set(b, CRO_SEGMENT_COUNT, 4);
    const uint32_t offsets[] = {IMG_CODE, IMG_RODATA, data, 0};
    const uint32_t sizes[] = {0x1000, data - IMG_RODATA, 0x100, 0x100};
    for (uint32_t i = 0; i < 4; ++i) {
        uint32_t at = segments + i * SEGMENT_RECORD;
        cro_set(b, at + SEGMENT_BASE, crs ? (i == 3 ? CRS_BSS : CRS_SEGMENT(i)) : offsets[i]);
        cro_set(b, at + SEGMENT_SIZE, crs && i == 1 ? 0x1000 : sizes[i]);
        cro_set(b, at + SEGMENT_TYPE, i);
    }
    cro_set(b, CRO_EXPORT_NAMED, named);
    cro_set(b, CRO_EXPORT_NAMED_COUNT, exports);
    cro_set(b, CRO_EXPORT_TREE, indexed);
    cro_set(b, CRO_EXPORT_INDEXED, indexed);
    cro_set(b, CRO_EXPORT_INDEXED_COUNT, module_exports ? 1 : 0);
    if (module_exports) cro_set(b, indexed, CRO_TAG(0, CRO_ENTRY));
    cro_set(b, CRO_EXPORT_STRINGS, strings);
    cro_set(b, CRO_EXPORT_STRINGS_SIZE, length + 6);
    memcpy(b + strings, name, length);
    memcpy(b + strings + length, "other", 6);
    for (uint32_t i = 0; i < exports; ++i) {
        cro_set(b, named + i * EXPORT_RECORD + EXPORT_NAME, strings + (i + 1 == exports ? 0 : length));
        cro_set(b, named + i * EXPORT_RECORD + EXPORT_TAG, CRO_TAG(0, CRO_ENTRY));
    }
    cro_set(b, CRO_IMPORT_MODULES, imports);
    cro_set(b, CRO_IMPORT_MODULE_COUNT, module_imports ? 1 : 0);
    cro_set(b, CRO_EXTERNAL, external);
    cro_set(b, CRO_EXTERNAL_COUNT, relocations);
    cro_set(b, CRO_IMPORT_NAMED, import_named);
    cro_set(b, CRO_IMPORT_NAMED_COUNT, !crs && !by_module ? 1 : 0);
    cro_set(b, CRO_IMPORT_INDEXED, import_indexed);
    cro_set(b, CRO_IMPORT_INDEXED_COUNT, module_imports ? 1 : 0);
    cro_set(b, CRO_IMPORT_ANONYMOUS, import_strings);
    cro_set(b, CRO_IMPORT_STRINGS, import_strings);
    cro_set(b, CRO_IMPORT_STRINGS_SIZE, length);
    memcpy(b + import_strings, name, length);
    if (!crs) {
        uint32_t import = by_module ? import_indexed : import_named;
        cro_set(b, import + IMPORT_NAME, by_module ? 0 : import_strings);
        cro_set(b, import + IMPORT_BATCH, external);
        if (by_module) {
            cro_set(b, imports + IMPORT_NAME, import_strings);
            cro_set(b, imports + IMPORT_MODULE_INDEXED, import_indexed);
            cro_set(b, imports + IMPORT_MODULE_INDEXED_COUNT, 1);
            cro_set(b, imports + IMPORT_MODULE_ANONYMOUS, import_strings);
        }
    }
    for (uint32_t i = 0; i < relocations; ++i) {
        uint32_t at = external + i * RELOCATION_RECORD;
        bool last = i + 1 == relocations;
        cro_set(b, at + RELOCATION_TAG, last ? CRO_TAG(2, 4) : CRO_TAG(2, 0));
        b[at + RELOCATION_TYPE] = 2;
        b[at + RELOCATION_END] = last;
        cro_set(b, at + RELOCATION_ADDEND, last ? LAST_ADDEND : 0);
    }
    cro_set(b, CRO_STATIC_ANONYMOUS, tail);
    cro_set(b, CRO_INTERNAL, tail);
    cro_set(b, CRO_STATIC, tail);
    cro_set(b, IMG_CODE, 0xe3a00001u);     /* mov r0, #1 */
    cro_set(b, IMG_CODE + 4, 0xeafffffeu); /* b . */
    return b;
}
int main(int argc, char** argv) {
    assert(argc == 4);
    const char* scenario = argv[1];
    unsigned command = (unsigned)strtoul(argv[2], NULL, 10);
    assert(command == 4 || command == 9);
    bool by_module = !strncmp(scenario, "module-", 7);
    const char* dash = strrchr(scenario, '-');
    assert(dash);
    uint32_t count = (uint32_t)strtoul(dash + 1, NULL, 10);
    bool missing_null = !strncmp(scenario, "missing-null-", 13);
    uint32_t length = !strncmp(scenario, "name-", 5) || by_module || missing_null ? count : 5;
    char name[513];
    assert(length < sizeof name);
    memset(name, 'x', length);
    name[length] = 0;
    bool exit_shim = !strcmp(scenario, "exit-shim");
    if (exit_shim) memcpy(name, "__aeabi_atexit", sizeof "__aeabi_atexit");
    unsigned char code[0x3000] = {0};
    SnxCtrImage executable = ctr_image(code, sizeof code, 4096);
    SnxCtrProcess* p = snx_ctr_process_new(&executable, !strcmp(argv[3], "new") ? SNX_CTR_NEW : SNX_CTR_OLD);
    assert(p);
    cro_map_heap(p, 0x900000);
    uint32_t session = cro_service(p);
    uint32_t crs_size;
    uint32_t crs_batch;
    unsigned char* crs =
        boundary_image(true, exit_shim ? "nnroAeabiAtexit_" : name, !strncmp(scenario, "exports-", 8) ? count : 1, 0,
                       by_module, &crs_size, &crs_batch);
    uint32_t cro_size;
    uint32_t cro_batch;
    unsigned char* cro =
        boundary_image(false, name, 0, !strncmp(scenario, "batch-", 6) ? count : 1, by_module, &cro_size, &cro_batch);
    const uint32_t crs_source = CRO_SOURCE + IMAGE_LIMIT;
    const uint32_t data = CRO_SOURCE + 0x800000;
    const uint32_t bss = data + 4096;
    assert(crs_size <= IMAGE_LIMIT && cro_size <= IMAGE_LIMIT);
    assert(snx_ctr_write_ram(p->space, crs_source, crs, crs_size) == SNX_CTR_MEM_OK);
    const uint32_t init[] = {crs_source, crs_size, CRO_CRS};
    assert(!cro_call(p, session, 1, init, 3, CTR_PROCESS_HANDLE, NULL));
    /* the terminator exists in RAM, outside the declared string table */
    if (missing_null) cro_set(cro, CRO_IMPORT_STRINGS_SIZE, length);
    assert(snx_ctr_write_ram(p->space, CRO_SOURCE, cro, cro_size) == SNX_CTR_MEM_OK);
    uint32_t load[LOAD_WORDS] = CRO_LOAD_DEFAULT;
    load[LOAD_SIZE] = cro_size;
    load[LOAD_DATA] = data;
    load[LOAD_BSS] = bss;
    uint32_t fixed;
    if (missing_null) {
        assert(cro_call(p, session, command, load, LOAD_WORDS, CTR_PROCESS_HANDLE, &fixed));
        assert(snx_ctr_query(p->space, CRO_MODULE, NULL, NULL) == SNX_CTR_MEM_UNMAPPED);
        goto done;
    }
    assert(!cro_call(p, session, command, load, LOAD_WORDS, CTR_PROCESS_HANDLE, &fixed));
    uint8_t state;
    assert(snx_ctr_read_ram(p->space, CRO_MODULE + cro_batch + RELOCATION_STATE, &state, 1) == SNX_CTR_MEM_OK);
    assert(state == 1); /* the batch is applied */
    /* LoadCRO_New applies the batch to .data before the data is transferred, and the older command after */
    if (command == 9) assert(ctr_word(p, data + 4) == CRS_SYMBOL + LAST_ADDEND);
    const uint32_t module[] = {CRO_MODULE};
    assert(!cro_call(p, session, 7, module, 1, CTR_PROCESS_HANDLE, NULL));
    /* unlinked, the word holds the module's unresolved symbol */
    assert(ctr_word(p, data + 4) == CRO_MODULE + IMG_CODE + CRO_UNRESOLVED + LAST_ADDEND);
    assert(!cro_call(p, session, 6, module, 1, CTR_PROCESS_HANDLE, NULL));
    assert(ctr_word(p, data + 4) == CRS_SYMBOL + LAST_ADDEND);
    const uint32_t unload[] = {CRO_MODULE, 0, CRO_SOURCE};
    assert(!cro_call(p, session, 5, unload, 3, CTR_PROCESS_HANDLE, NULL));
    assert(snx_ctr_query(p->space, CRO_MODULE, NULL, NULL) == SNX_CTR_MEM_UNMAPPED);
    puts(scenario);
done:
    free(cro);
    free(crs);
    snx_ctr_process_free(p);
    return 0;
}
