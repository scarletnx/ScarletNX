/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "support/3ds_cro.h"
#include "3ds/services/cro.h"
#include "3ds/services/ldr.h"

enum {
    SCN_FIX,
    SCN_HANDLE,
    SCN_SHUTDOWN,
    SCN_OVERLAP,
    SCN_BSS,
    SCN_BOUNDS,
    SCN_TARGET,
    SCN_BATCH,
    SCN_ROLLBACK,
    SCN_DATA,
    SCN_REUSE,
    SCN_CRS_INTERNAL,
    SCN_BSS_SOURCE,
    SCN_DATA_SOURCE,
    SCN_LIFETIME,
    SCN_LINK,
    SCN_LATE_FAILURE,
    SCN_METADATA,
    SCN_FIX0,
    SCN_FIX1,
    SCN_FIX2,
    SCN_FIX3,
    SCN_NO_CODE,
    SCN_RELATIVE_DATA,
    SCN_PRIVATE_DATA,
    SCN_PID_DESCRIPTOR,
    SCN_SEGMENT_OVERLAP,
    SCN_FILE_SIZE,
    SCN_NAME,
    SCN_STATIC_BOUND,
    SCN_STATIC_NO_JIT,
    SCN_INDEXED,
    SCN_ANONYMOUS,
    SCN_COPIED_PROCESS,
    SCN_OLD_COMMAND,
    SCN_CRS_EMPTY_SEGMENT,
    SCN_CRS_REPEAT_SEGMENT,
    SCN_CRS_UNMAPPED_SEGMENT,
    SCN_CRS_UNMAPPED_RELOCATION,
    SCN_SAME_BUFFER,
    SCN_SAME_BUFFER_ZERO_DATA,
    SCN_CODE_RANGE,
    SCN_COUNT
};
static const char* const names[SCN_COUNT] = {
    [SCN_FIX] = "fix",
    [SCN_HANDLE] = "handle",
    [SCN_SHUTDOWN] = "shutdown",
    [SCN_OVERLAP] = "overlap",
    [SCN_BSS] = "bss",
    [SCN_BOUNDS] = "bounds",
    [SCN_TARGET] = "target",
    [SCN_BATCH] = "batch",
    [SCN_ROLLBACK] = "rollback",
    [SCN_DATA] = "data",
    [SCN_REUSE] = "reuse",
    [SCN_CRS_INTERNAL] = "crs-internal",
    [SCN_BSS_SOURCE] = "bss-source",
    [SCN_DATA_SOURCE] = "data-source",
    [SCN_LIFETIME] = "lifetime",
    [SCN_LINK] = "link",
    [SCN_LATE_FAILURE] = "late-failure",
    [SCN_METADATA] = "metadata",
    [SCN_FIX0] = "fix0",
    [SCN_FIX1] = "fix1",
    [SCN_FIX2] = "fix2",
    [SCN_FIX3] = "fix3",
    [SCN_NO_CODE] = "no-code",
    [SCN_RELATIVE_DATA] = "relative-data",
    [SCN_PRIVATE_DATA] = "private-data",
    [SCN_PID_DESCRIPTOR] = "pid-descriptor",
    [SCN_SEGMENT_OVERLAP] = "segment-overlap",
    [SCN_FILE_SIZE] = "file-size",
    [SCN_NAME] = "name",
    [SCN_STATIC_BOUND] = "static-bound",
    [SCN_STATIC_NO_JIT] = "static-no-jit",
    [SCN_INDEXED] = "indexed",
    [SCN_ANONYMOUS] = "anonymous",
    [SCN_COPIED_PROCESS] = "copied-process",
    [SCN_OLD_COMMAND] = "old-command",
    [SCN_CRS_EMPTY_SEGMENT] = "crs-empty-segment",
    [SCN_CRS_REPEAT_SEGMENT] = "crs-repeat-segment",
    [SCN_CRS_UNMAPPED_SEGMENT] = "crs-unmapped-segment",
    [SCN_CRS_UNMAPPED_RELOCATION] = "crs-unmapped-relocation",
    [SCN_SAME_BUFFER] = "same-buffer",
    [SCN_SAME_BUFFER_ZERO_DATA] = "same-buffer-zero-data",
    [SCN_CODE_RANGE] = "code-range",
};
/* where fix1 to fix3 move the tables, behind the segments the loader keeps */
enum {
    FIX_EXPORT_NAMED = 0x3000,
    FIX_EXPORT_STRINGS = 0x3008,
    FIX_IMPORT_MODULES = 0x3010,
    FIX_EXTERNAL = 0x3020,
    FIX_IMPORT_NAMED = 0x3030,
    FIX_IMPORT_STRINGS = 0x3040,
    FIX_STATIC_ANONYMOUS = 0x3050,
    FIX_INTERNAL = 0x3060,
    FIX_STATIC = 0x3070,
    FIX_DATA = 0x3f00
};
#define CRS_EXPORT_ADDRESS CRS_SYMBOL /* what the export "entry" of the CRS resolves to */
#define DATA_MARK          0x12345678u
#define BSS_MARK           0xa5a5a5a5u
#define RO_INVALID_IMAGE   0xd8601803u /* the result of ldr:ro for an image that it refuses */
/* the bytes that a CRS segment holds beyond its code: mapped, and unmapped for the scenario that checks it */
#define CRS_MAPPED_EXTRA   0x200800u
#define CRS_UNMAPPED_EXTRA 0x04000000u

static int find_scenario(const char* name) {
    for (int s = 0; s < SCN_COUNT; ++s)
        if (!strcmp(name, names[s])) return s;
    assert(!"unknown scenario");
    return -1;
}
static bool extra_crs_segment(int scenario) {
    return scenario >= SCN_CRS_EMPTY_SEGMENT && scenario <= SCN_CRS_UNMAPPED_RELOCATION;
}
static bool fix_level_scenario(int scenario) { return scenario >= SCN_FIX0 && scenario <= SCN_FIX3; }
static bool imports_by_module(int scenario) { return scenario == SCN_INDEXED || scenario == SCN_ANONYMOUS; }
static bool same_buffer(int scenario) { return scenario == SCN_SAME_BUFFER || scenario == SCN_SAME_BUFFER_ZERO_DATA; }
/* the scenarios whose LoadCRO must fail and leave the process as it was */
static bool load_fails(int scenario) {
    static const int failing[] = {SCN_OVERLAP,      SCN_BSS,          SCN_BOUNDS,          SCN_TARGET,
                                  SCN_BATCH,        SCN_ROLLBACK,     SCN_BSS_SOURCE,      SCN_DATA_SOURCE,
                                  SCN_LATE_FAILURE, SCN_METADATA,     SCN_SEGMENT_OVERLAP, SCN_FILE_SIZE,
                                  SCN_NAME,         SCN_STATIC_BOUND, SCN_STATIC_NO_JIT};
    for (uint32_t i = 0; i < sizeof failing / sizeof *failing; ++i)
        if (failing[i] == scenario) return true;
    return false;
}
/* The CRS the process starts with, changed for the scenarios that need another one */
static void make_crs(unsigned char* cro, int scenario) {
    cro_image(cro, true);
    if (extra_crs_segment(scenario)) {
        bool empty = scenario == SCN_CRS_EMPTY_SEGMENT;
        bool unmapped = scenario == SCN_CRS_UNMAPPED_SEGMENT || scenario == SCN_CRS_UNMAPPED_RELOCATION;
        cro_set(cro, CRO_SEGMENT_COUNT, 5);
        cro_set(cro, IMG_SEGMENT(4) + SEGMENT_BASE, unmapped ? CRS_UNMAPPED_EXTRA : CRS_MAPPED_EXTRA);
        cro_set(cro, IMG_SEGMENT(4) + SEGMENT_SIZE, empty ? 0 : 0x100);
        cro_set(cro, IMG_SEGMENT(4) + SEGMENT_TYPE, 1); /* tags identify segments by index; a type can occur again */
        if (!empty) cro_set(cro, IMG_SEGMENT(1) + SEGMENT_SIZE, 0x800);
        if (scenario == SCN_CRS_UNMAPPED_RELOCATION) {
            cro_set(cro, CRO_INTERNAL_COUNT, 1);
            cro_set(cro, IMG_INTERNAL + RELOCATION_TAG, CRO_TAG(4, 0)); /* the unmapped fifth segment, as a target */
            cro[IMG_INTERNAL + RELOCATION_TYPE] = 2;
        }
    }
    if (scenario == SCN_CRS_INTERNAL) {
        cro_set(cro, CRO_INTERNAL_COUNT, 1);
        cro_set(cro, IMG_INTERNAL + RELOCATION_TAG, CRO_TAG(1, 0x40));
        cro[IMG_INTERNAL + RELOCATION_TYPE] = 2;
        cro[IMG_INTERNAL + RELOCATION_SEGMENT] = 0;
        cro_set(cro, IMG_INTERNAL + RELOCATION_ADDEND, 0x44);
    }
    if (scenario == SCN_INDEXED) {
        cro_set(cro, CRO_EXPORT_NAMED_COUNT, 0);
        cro_set(cro, CRO_EXPORT_INDEXED_COUNT, 1);
        cro_set(cro, IMG_EXPORT_STRINGS, CRO_TAG(0, CRO_ENTRY));
        cro_set(cro, CRO_EXPORT_STRINGS, IMG_EXPORT_STRINGS + 4);
        memcpy(cro + IMG_EXPORT_STRINGS + 4, "entry", 6);
    }
}
/* The fix levels keep less of the tables, so these tests put them behind the segments */
static void move_tables(unsigned char* cro) {
    cro_set(cro, CRO_EXPORT_NAMED, FIX_EXPORT_NAMED);
    cro_set(cro, CRO_EXPORT_NAMED_COUNT, 1);
    cro_set(cro, CRO_EXPORT_TREE, FIX_EXPORT_STRINGS);
    cro_set(cro, CRO_EXPORT_INDEXED, FIX_EXPORT_STRINGS);
    cro_set(cro, CRO_EXPORT_STRINGS, FIX_EXPORT_STRINGS);
    memcpy(cro + FIX_EXPORT_STRINGS, "entry", 6);
    cro_set(cro, FIX_EXPORT_NAMED + EXPORT_NAME, FIX_EXPORT_STRINGS);
    cro_set(cro, FIX_EXPORT_NAMED + EXPORT_TAG, CRO_TAG(0, CRO_ENTRY));
    cro_set(cro, CRO_IMPORT_MODULES, FIX_IMPORT_MODULES);
    cro_set(cro, CRO_EXTERNAL, FIX_EXTERNAL);
    cro_set(cro, CRO_IMPORT_NAMED, FIX_IMPORT_NAMED);
    cro_set(cro, CRO_IMPORT_INDEXED, FIX_IMPORT_STRINGS);
    cro_set(cro, CRO_IMPORT_ANONYMOUS, FIX_IMPORT_STRINGS);
    cro_set(cro, CRO_IMPORT_STRINGS, FIX_IMPORT_STRINGS);
    memcpy(cro + FIX_IMPORT_STRINGS, "entry", 6);
    cro_set(cro, CRO_STATIC_ANONYMOUS, FIX_STATIC_ANONYMOUS);
    cro_set(cro, CRO_INTERNAL, FIX_INTERNAL);
    cro_set(cro, CRO_STATIC, FIX_STATIC);
    cro_set(cro, FIX_EXTERNAL + RELOCATION_TAG, CRO_TAG(2, 0));
    cro[FIX_EXTERNAL + RELOCATION_TYPE] = 2;
    cro[FIX_EXTERNAL + RELOCATION_END] = 1;
    cro_set(cro, FIX_IMPORT_NAMED + IMPORT_NAME, FIX_IMPORT_STRINGS);
    cro_set(cro, FIX_IMPORT_NAMED + IMPORT_BATCH, FIX_EXTERNAL);
    cro_set(cro, CRO_DATA_OFFSET, FIX_DATA);
    cro_set(cro, IMG_SEGMENT(2) + SEGMENT_BASE, FIX_DATA);
}
/* The module to load, changed for the scenarios that make its load fail or take another path */
static void make_module(unsigned char* cro, uint32_t* load, int scenario) {
    cro_image(cro, false);
    if (scenario == SCN_SAME_BUFFER_ZERO_DATA) {
        cro_set(cro, CRO_EXTERNAL_COUNT, 0);
        cro_set(cro, CRO_IMPORT_NAMED_COUNT, 0);        /* no named import that would point at it */
        cro_set(cro, CRO_DATA_SIZE, 0);                 /* an empty .data segment: the header size */
        cro_set(cro, IMG_SEGMENT(2) + SEGMENT_SIZE, 0); /* and the segment entry agree */
    }
    if (scenario == SCN_OVERLAP) load[LOAD_ADDRESS] = CRO_SOURCE;
    if (scenario == SCN_BSS) load[LOAD_BSS] = 0x07000000;
    if (scenario == SCN_BSS_SOURCE) load[LOAD_BSS] = CRO_SOURCE;
    if (scenario == SCN_DATA_SOURCE) load[LOAD_DATA] = CRO_SOURCE;
    if (scenario == SCN_BOUNDS) cro_set(cro, CRO_SEGMENT_COUNT, 0x40000000u);
    if (scenario == SCN_TARGET) cro_set(cro, IMG_EXTERNAL + RELOCATION_TAG, CRO_TAG(2, 0xff));   /* .data + 255 */
    if (scenario == SCN_BATCH) cro_set(cro, IMG_IMPORT_NAMED + IMPORT_BATCH, IMG_EXTERNAL + 12); /* one past */
    if (scenario == SCN_ROLLBACK) cro[IMG_EXTERNAL + RELOCATION_TYPE] = 255;
    if (scenario == SCN_SEGMENT_OVERLAP) cro_set(cro, IMG_SEGMENT(1) + SEGMENT_BASE, IMG_CODE);
    if (scenario == SCN_FILE_SIZE) cro_set(cro, CRO_FILE_SIZE, 0x3001);
    if (scenario == SCN_NAME) memset(cro + IMG_NAME, 'a', 8);
    if (scenario == SCN_STATIC_BOUND) cro_add_static_relocation(cro, CRO_TAG(2, 0xff), 0);
    if (scenario == SCN_STATIC_NO_JIT) cro_add_static_relocation(cro, 0, 0);
    if (imports_by_module(scenario)) {
        cro_set(cro, CRO_IMPORT_MODULE_COUNT, 1);
        cro_set(cro, CRO_IMPORT_NAMED_COUNT, 0);
        cro_set(cro, CRO_IMPORT_STRINGS_SIZE, 9);
        cro_set(cro, IMG_IMPORT_MODULES + IMPORT_NAME, IMG_IMPORT_STRINGS);
        memcpy(cro + IMG_IMPORT_STRINGS, "|static|", 9);
        cro_set(cro, CRO_IMPORT_INDEXED, IMG_IMPORT_NAMED);
        cro_set(cro, CRO_IMPORT_ANONYMOUS, IMG_IMPORT_NAMED);
        if (scenario == SCN_INDEXED) {
            cro_set(cro, CRO_IMPORT_INDEXED_COUNT, 1);
            cro_set(cro, IMG_IMPORT_MODULES + IMPORT_MODULE_INDEXED, IMG_IMPORT_NAMED);
            cro_set(cro, IMG_IMPORT_MODULES + IMPORT_MODULE_INDEXED_COUNT, 1);
            cro_set(cro, IMG_IMPORT_NAMED, 0); /* the first indexed export */
        } else {
            cro_set(cro, CRO_IMPORT_ANONYMOUS_COUNT, 1);
            cro_set(cro, IMG_IMPORT_MODULES + IMPORT_MODULE_ANONYMOUS, IMG_IMPORT_NAMED);
            cro_set(cro, IMG_IMPORT_MODULES + IMPORT_MODULE_ANONYMOUS_COUNT, 1);
            cro_set(cro, IMG_IMPORT_NAMED, CRO_TAG(0, CRO_ENTRY));
        }
        cro_set(cro, IMG_IMPORT_NAMED + IMPORT_BATCH, IMG_EXTERNAL);
    }
    if (scenario == SCN_NO_CODE) {
        cro_set(cro, CRO_CODE_SIZE, 0);
        cro_set(cro, IMG_SEGMENT(0) + SEGMENT_SIZE, 0);
        cro_set(cro, CRO_EXTERNAL_COUNT, 0);
        cro_set(cro, CRO_IMPORT_NAMED_COUNT, 0);
    }
    if (scenario == SCN_LATE_FAILURE) {
        /* an exit shim import that no export answers */
        memcpy(cro + IMG_IMPORT_STRINGS, "__aeabi_atexit", 15);
        cro_set(cro, CRO_IMPORT_STRINGS_SIZE, 15);
        cro_add_static_relocation(cro, CRO_TAG(1, 0x40), 0);
    }
    if (scenario == SCN_RELATIVE_DATA) cro[IMG_EXTERNAL + RELOCATION_TYPE] = 3;
    /* RODATA points at an import record */
    if (scenario == SCN_METADATA) cro_set(cro, IMG_EXTERNAL + RELOCATION_TAG, CRO_TAG(1, 0x110));
    if (fix_level_scenario(scenario)) {
        load[LOAD_FIX] = (uint32_t)(scenario - SCN_FIX0);
        move_tables(cro);
    }
}
/* A failed LoadCRO leaves the mappings, the data and bss, the CRS and the source image as they were */
static void check_load_failed(SnxCtrProcess* p, const uint32_t* load, const unsigned char* cro, uint32_t shared,
                              uint32_t old_rodata, const unsigned char* data_before, const unsigned char* bss_before) {
    if (load[LOAD_ADDRESS] == CRO_MODULE)
        assert(snx_ctr_query(p->space, CRO_MODULE, NULL, NULL) == SNX_CTR_MEM_UNMAPPED);
    assert(p->physical_shared == shared);
    assert(cro_word(p, CRO_DATA) == DATA_MARK && cro_word(p, CRO_BSS) == BSS_MARK);
    assert(cro_word(p, CRS_SEGMENT(1) + 0x40) == old_rodata);
    unsigned char output[256];
    assert(snx_ctr_read_ram(p->space, CRO_DATA, output, sizeof output) == SNX_CTR_MEM_OK);
    assert(!memcmp(data_before, output, sizeof output));
    assert(snx_ctr_read_ram(p->space, CRO_BSS, output, sizeof output) == SNX_CTR_MEM_OK);
    assert(!memcmp(bss_before, output, sizeof output));
    unsigned char after[CRO_IMAGE_SIZE];
    assert(snx_ctr_read_ram(p->space, CRO_SOURCE, after, sizeof after) == SNX_CTR_MEM_OK);
    assert(!memcmp(cro, after, sizeof after));
}
/* The unload of a module that shared its buffer with the image: the buffer is the image again, and loads again */
static void check_same_buffer(SnxCtrProcess* p, uint32_t session, int scenario, uint32_t* load) {
    bool zero_data = scenario == SCN_SAME_BUFFER_ZERO_DATA;
    uint8_t byte = 0;
    assert(p->ldr_count == 1 && p->ldr_modules[0].address == CRO_MODULE && p->ldr_modules[0].buffer == CRO_SOURCE);
    assert(cro_word(p, CRO_SOURCE + IMG_SEGMENT(2) + SEGMENT_BASE) == (zero_data ? CRO_MODULE + IMG_DATA : CRO_DATA));
    assert(snx_ctr_read_ram(p->space, CRO_SOURCE + CRO_NEXT, &byte, 1) == SNX_CTR_MEM_OK);
    assert(snx_ctr_write_ram(p->space, CRO_SOURCE + CRO_NEXT, &byte, 1) == SNX_CTR_MEM_PERMISSION);
    const uint32_t unload[] = {CRO_MODULE, 0, CRO_SOURCE};
    assert(!cro_call(p, session, 5, unload, 3, CTR_PROCESS_HANDLE, NULL));
    assert(!p->ldr_count && snx_ctr_query(p->space, CRO_MODULE, NULL, NULL) == SNX_CTR_MEM_UNMAPPED);
    /* the .data entry and DATA_OFFSET are the file offset again */
    assert(cro_word(p, CRO_SOURCE + IMG_SEGMENT(2) + SEGMENT_BASE) == IMG_DATA);
    assert(cro_word(p, CRO_SOURCE + CRO_DATA_OFFSET) == IMG_DATA);
    unsigned char before[CRO_IMAGE_SIZE];
    assert(snx_ctr_read_ram(p->space, CRO_SOURCE, before, sizeof before) == SNX_CTR_MEM_OK);
    /* Writing back the value just read keeps the buffer byte-identical while proving that the unloaded mapping
       is writable. */
    assert(snx_ctr_read_ram(p->space, CRO_SOURCE + CRO_NEXT, &byte, 1) == SNX_CTR_MEM_OK);
    assert(snx_ctr_write_ram(p->space, CRO_SOURCE + CRO_NEXT, &byte, 1) == SNX_CTR_MEM_OK);
    unsigned char after[CRO_IMAGE_SIZE];
    assert(snx_ctr_read_ram(p->space, CRO_SOURCE, after, sizeof after) == SNX_CTR_MEM_OK);
    assert(!memcmp(before, after, sizeof before));
    uint32_t fixed = 0;
    assert(!cro_call(p, session, 9, load, LOAD_WORDS, CTR_PROCESS_HANDLE, &fixed));
    assert(p->ldr_count == 1 && fixed == IMG_DATA);
    assert(cro_word(p, CRO_SOURCE + IMG_SEGMENT(2) + SEGMENT_BASE) == (zero_data ? CRO_MODULE + IMG_DATA : CRO_DATA));
    assert(snx_ctr_read_ram(p->space, CRO_SOURCE + CRO_NEXT, &byte, 1) == SNX_CTR_MEM_OK);
    assert(snx_ctr_write_ram(p->space, CRO_SOURCE + CRO_NEXT, &byte, 1) == SNX_CTR_MEM_PERMISSION);
}
/* The pages of a code segment that ends at the end of the address space, and those of segments that leave it. A module
   without code has no pages to report, and that is not an error. */
static void check_code_range(SnxCtrProcess* p) {
    static const struct {
        uint32_t offset, bytes, begin, size;
        bool fits;
    } cases[] = {
        {0x1234, 0x10, 0x1000, 0x1000, true},
        {0x1234, 0, 0, 0, true},
        {0xfffff000u, 0x1000, 0xfffff000u, 0x1000, true},
        {0xfffff001u, 0xfff, 0xfffff000u, 0x1000, true},
        {0xfffff800u, 0x1000, 0, 0, false},
        {0, 0xffffffffu, 0, 0, false},
    };
    unsigned char image[CRO_IMAGE_SIZE];
    for (uint32_t i = 0; i < sizeof cases / sizeof *cases; ++i) {
        cro_image(image, false);
        cro_set(image, CRO_SEGMENTS, CRO_SOURCE + IMG_SEGMENTS); /* rebased: the table is at its address */
        cro_set(image, IMG_SEGMENT(0) + SEGMENT_BASE, cases[i].offset);
        cro_set(image, IMG_SEGMENT(0) + SEGMENT_SIZE, cases[i].bytes);
        assert(snx_ctr_write_ram(p->space, CRO_SOURCE, image, sizeof image) == SNX_CTR_MEM_OK);
        uint32_t begin = 0;
        uint32_t size = 0;
        assert(snx_ctr_cro_code(p, CRO_SOURCE, &begin, &size) == cases[i].fits);
        if (cases[i].fits) assert(begin == cases[i].begin && size == cases[i].size);
    }
}

int main(int argc, char** argv) {
    assert(argc == 2 || argc == 3);
    const char* name = argv[1];
    int scenario = find_scenario(name);
    unsigned char code[0x8000] = {0};
    SnxCtrImage executable = {.segment = {{0x100000, 1, 16}, {0x200000, 1, 4}, {0x300000, 6, 0x6000}},
                              .stack_size = 0x4000,
                              .main_priority = 0x30,
                              .bss_size = 4096,
                              .code = code,
                              .code_size = sizeof code};
    SnxCtrProcess* p = snx_ctr_process_new(&executable, argc == 3 ? SNX_CTR_NEW : SNX_CTR_OLD);
    assert(p);
    cro_map_heap(p, 0xa000);
    uint32_t session = cro_service(p);
    if (scenario == SCN_CODE_RANGE) {
        check_code_range(p);
        goto done;
    }
    unsigned char cro[CRO_IMAGE_SIZE];
    make_crs(cro, scenario);
    uint32_t crs_source = scenario == SCN_PRIVATE_DATA ? 0x300000u : CRO_CRS_SOURCE;
    assert(snx_ctr_write_ram(p->space, crs_source, cro, sizeof cro) == SNX_CTR_MEM_OK);
    const uint32_t init[] = {crs_source, CRO_IMAGE_SIZE, CRO_CRS};
    if (scenario == SCN_HANDLE || scenario == SCN_PID_DESCRIPTOR) {
        assert(cro_call(p, session, 1, init, 3, scenario == SCN_HANDLE ? 0 : CRO_PID_DESCRIPTOR, NULL));
        assert(!p->ldr_crs);
        goto done;
    }
    uint32_t process_handle = CTR_PROCESS_HANDLE;
    if (scenario == SCN_COPIED_PROCESS) {
        p->cpu.state.cpu.r[1] = CTR_PROCESS_HANDLE;
        assert(snx_ctr_svc(p, SVC_DUPLICATE_HANDLE) == SNX_CTR_SVC_RETURN && !p->cpu.state.cpu.r[0]);
        process_handle = p->cpu.state.cpu.r[1];
    }
    if (scenario == SCN_CRS_UNMAPPED_RELOCATION) {
        assert(cro_call(p, session, 1, init, 3, process_handle, NULL) == RO_INVALID_IMAGE);
        assert(!p->ldr_crs && snx_ctr_query(p->space, CRO_CRS, NULL, NULL) == SNX_CTR_MEM_UNMAPPED);
        unsigned char unchanged[CRO_IMAGE_SIZE];
        assert(snx_ctr_read_ram(p->space, crs_source, unchanged, sizeof unchanged) == SNX_CTR_MEM_OK);
        assert(!memcmp(cro, unchanged, sizeof cro));
        goto done;
    }
    assert(!cro_call(p, session, 1, init, 3, process_handle, NULL));
    const uint32_t crs_shutdown[] = {crs_source};
    if (extra_crs_segment(scenario)) {
        uint32_t extra_base = scenario == SCN_CRS_UNMAPPED_SEGMENT ? CRS_UNMAPPED_EXTRA : CRS_MAPPED_EXTRA;
        assert(cro_word(p, CRO_CRS + IMG_SEGMENT(4) + SEGMENT_BASE) == extra_base);
        assert(!cro_call(p, session, 8, crs_shutdown, 1, CTR_PROCESS_HANDLE, NULL));
        assert(cro_word(p, crs_source + IMG_SEGMENT(4) + SEGMENT_BASE) == extra_base);
        goto done;
    }
    if (scenario == SCN_COPIED_PROCESS) goto done;
    if (scenario == SCN_PRIVATE_DATA) {
        uint32_t source_pa;
        uint32_t module_pa;
        assert(snx_ctr_query(p->space, crs_source, &source_pa, NULL) == SNX_CTR_MEM_OK);
        assert(snx_ctr_query(p->space, CRO_CRS, &module_pa, NULL) == SNX_CTR_MEM_OK && source_pa == module_pa);
        assert(!cro_call(p, session, 8, crs_shutdown, 1, CTR_PROCESS_HANDLE, NULL));
        goto done;
    }
    if (scenario == SCN_CRS_INTERNAL) {
        assert(cro_word(p, CRS_SEGMENT(1) + 0x40) == CRS_EXPORT_ADDRESS);
        goto done;
    }
    if (scenario == SCN_SHUTDOWN) {
        assert(!cro_call(p, session, 8, crs_shutdown, 1, CTR_PROCESS_HANDLE, NULL));
        assert(snx_ctr_query(p->space, CRO_CRS, NULL, NULL) == SNX_CTR_MEM_UNMAPPED);
        assert(!cro_call(p, session, 1, init, 3, CTR_PROCESS_HANDLE, NULL));
        goto done;
    }
    uint32_t load[LOAD_WORDS] = CRO_LOAD_DEFAULT;
    make_module(cro, load, scenario);
    assert(snx_ctr_write_ram(p->space, CRO_SOURCE, cro, sizeof cro) == SNX_CTR_MEM_OK);
    cro_put(p, CRO_DATA, DATA_MARK);
    cro_put(p, CRO_BSS, BSS_MARK);
    unsigned char data_before[256];
    unsigned char bss_before[256];
    assert(snx_ctr_read_ram(p->space, CRO_DATA, data_before, sizeof data_before) == SNX_CTR_MEM_OK);
    assert(snx_ctr_read_ram(p->space, CRO_BSS, bss_before, sizeof bss_before) == SNX_CTR_MEM_OK);
    uint32_t old_rodata = cro_word(p, CRS_SEGMENT(1) + 0x40);
    uint32_t shared = p->physical_shared;
    uint32_t fixed = 0;
    unsigned command = scenario == SCN_OLD_COMMAND ? 4 : 9;
    uint32_t result = cro_call(p, session, command, load, LOAD_WORDS, CTR_PROCESS_HANDLE, &fixed);
    if (load_fails(scenario)) {
        assert(result);
        check_load_failed(p, load, cro, shared, old_rodata, data_before, bss_before);
        goto done;
    }
    assert(!result);
    const uint32_t unload[] = {CRO_MODULE, 0, CRO_SOURCE};
    const uint32_t module[] = {CRO_MODULE};
    if (scenario == SCN_FIX) assert(fixed == IMG_DATA);
    if (fix_level_scenario(scenario)) {
        assert(fixed == (scenario == SCN_FIX3 ? FIX_EXPORT_NAMED : CRO_IMAGE_SIZE));
        assert(!cro_call(p, session, 5, unload, 3, CTR_PROCESS_HANDLE, NULL));
    }
    if (scenario == SCN_OLD_COMMAND) {
        /* The original command imports into external DATA before transfer, which overwrites it. */
        assert(cro_word(p, CRO_DATA) == 0 && cro_word(p, CRO_SOURCE + IMG_DATA) == 0);
    }
    if (imports_by_module(scenario)) assert(cro_word(p, CRO_DATA) == CRS_EXPORT_ADDRESS);
    if (scenario == SCN_DATA || scenario == SCN_RELATIVE_DATA) {
        uint32_t expected = scenario == SCN_RELATIVE_DATA ? CRS_EXPORT_ADDRESS - CRO_DATA : CRS_EXPORT_ADDRESS;
        assert(cro_word(p, CRO_DATA) == expected);
        assert(cro_word(p, CRO_SOURCE + IMG_DATA) == cro_word(p, CRO_DATA));
    }
    if (scenario == SCN_LIFETIME) {
        uint32_t output;
        uint8_t byte = 1;
        assert(snx_ctr_write_ram(p->space, CRO_SOURCE + CRO_NEXT, &byte, 1) == SNX_CTR_MEM_PERMISSION);
        assert(snx_ctr_write_ram(p->space, CRO_MODULE + CRO_NEXT, &byte, 1) == SNX_CTR_MEM_PERMISSION);
        assert(snx_ctr_kernel_memory(p, MEMOP_PROTECT, CRO_SOURCE, 0, 0x1000, SNX_CTR_READ | SNX_CTR_WRITE, &output));
        assert(snx_ctr_kernel_memory(p, MEMOP_FREE, CRO_SOURCE, 0, 0x1000, 0, &output));
        assert(snx_ctr_query(p->space, CRO_MODULE + fixed, NULL, NULL) == SNX_CTR_MEM_UNMAPPED);
        assert(!snx_ctr_kernel_memory(p, MEMOP_FREE, CRO_SOURCE + fixed, 0, CRO_IMAGE_SIZE - fixed, 0, &output));
        assert(!cro_call(p, session, 5, unload, 3, CTR_PROCESS_HANDLE, NULL));
        assert(!snx_ctr_kernel_memory(p, MEMOP_FREE, CRO_SOURCE, 0, fixed, 0, &output));
    }
    if (scenario == SCN_LINK) {
        assert(!cro_call(p, session, 7, module, 1, CTR_PROCESS_HANDLE, NULL));
        assert(cro_word(p, CRO_DATA) == CRO_MODULE + IMG_CODE + CRO_UNRESOLVED);
        assert(!cro_call(p, session, 6, module, 1, CTR_PROCESS_HANDLE, NULL));
        assert(cro_word(p, CRO_DATA) == CRS_EXPORT_ADDRESS);
    }
    if (scenario == SCN_REUSE) {
        assert(!cro_call(p, session, 5, unload, 3, CTR_PROCESS_HANDLE, NULL));
        assert(snx_ctr_query(p->space, CRO_MODULE, NULL, NULL) == SNX_CTR_MEM_UNMAPPED);
        cro_image(cro, false);
        assert(snx_ctr_write_ram(p->space, CRO_SOURCE, cro, sizeof cro) == SNX_CTR_MEM_OK);
        assert(!cro_call(p, session, 9, load, LOAD_WORDS, CTR_PROCESS_HANDLE, &fixed));
        assert(p->physical_shared == shared);
    }
    if (same_buffer(scenario)) check_same_buffer(p, session, scenario, load);
done:
    snx_ctr_process_free(p);
    puts(name);
    return 0;
}
