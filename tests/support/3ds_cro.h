/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_TEST_CRO_H
#define SNX_TEST_CRO_H
#include "3ds/runtime/internal.h"
#include "3ds/ipc/ctr_ipc.h"
#include "3ds_guest.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Where the tests put the private heap buffers and the modules the loader maps from them */
#define CRO_SOURCE                        0x08000000u
#define CRO_CRS_SOURCE                    (CRO_SOURCE + 0x4000u)
#define CRO_MODULE                        0x01000000u
#define CRO_CRS                           0x02000000u
#define CRO_DATA                          (CRO_SOURCE + 0x8000u)
#define CRO_BSS                           (CRO_SOURCE + 0x9000u)
#define CRO_IMAGE_SIZE                    0x4000u
/* a CRO image starts its header this far in, and the header starts with the magic word "CRO0" */
#define CRO_HEADER                        0x80u
#define CRO_MAGIC                         0x304f5243u
/* the code offset that the on-unresolved tag names, and the export "entry", which is this far into the code */
#define CRO_UNRESOLVED                    0x80u
#define CRO_ENTRY                         0x44u
/* the bss segment of the static module's CRS, which lies apart from its code and data */
#define CRS_BSS                           0x301000u
/* the sizes of the records of the tables */
#define SEGMENT_RECORD                    12u
#define RELOCATION_RECORD                 12u
#define EXPORT_RECORD                     8u
#define IMPORT_RECORD                     8u
#define IMPORT_MODULE_RECORD              20u
/* the SVC numbers that the tests call */
#define SVC_DUPLICATE_HANDLE              0x27u
#define SVC_CONNECT_TO_PORT               0x2du
/* an IPC command header: the command, the normal words after it, and the translate words after those */
#define IPC_HEADER(id, normal, translate) ((uint32_t)(id) << 16 | (uint32_t)(normal) << 6 | (uint32_t)(translate))
/* the translate descriptors: the process id, and the move of one handle */
#define IPC_PROCESS_ID                    0x20u
#define IPC_MOVE_HANDLE                   0x10u
/* sent in place of the process handle: the process id descriptor */
#define CRO_PID_DESCRIPTOR                UINT32_MAX
/* a segment tag: the segment index in the low four bits, the offset in it above them */
#define CRO_TAG(segment, offset)          ((uint32_t)(offset) << 4 | (segment))

/* Byte offsets of the header fields of a CRO image, which starts its header at 0x80 */
enum {
    CRO_NEXT = 0x88,
    CRO_FILE_SIZE = 0x90,
    CRO_BSS_SIZE = 0x94,
    CRO_ON_UNRESOLVED = 0xac,
    CRO_CODE_OFFSET = 0xb0,
    CRO_CODE_SIZE = 0xb4,
    CRO_DATA_OFFSET = 0xb8,
    CRO_DATA_SIZE = 0xbc,
    CRO_MODULE_NAME = 0xc0,
    CRO_MODULE_NAME_SIZE = 0xc4,
    CRO_SEGMENTS = 0xc8,
    CRO_SEGMENT_COUNT = 0xcc,
    CRO_EXPORT_NAMED = 0xd0,
    CRO_EXPORT_NAMED_COUNT = 0xd4,
    CRO_EXPORT_INDEXED = 0xd8,
    CRO_EXPORT_INDEXED_COUNT = 0xdc,
    CRO_EXPORT_STRINGS = 0xe0,
    CRO_EXPORT_STRINGS_SIZE = 0xe4,
    CRO_EXPORT_TREE = 0xe8,
    CRO_IMPORT_MODULES = 0xf0,
    CRO_IMPORT_MODULE_COUNT = 0xf4,
    CRO_EXTERNAL = 0xf8,
    CRO_EXTERNAL_COUNT = 0xfc,
    CRO_IMPORT_NAMED = 0x100,
    CRO_IMPORT_NAMED_COUNT = 0x104,
    CRO_IMPORT_INDEXED = 0x108,
    CRO_IMPORT_INDEXED_COUNT = 0x10c,
    CRO_IMPORT_ANONYMOUS = 0x110,
    CRO_IMPORT_ANONYMOUS_COUNT = 0x114,
    CRO_IMPORT_STRINGS = 0x118,
    CRO_IMPORT_STRINGS_SIZE = 0x11c,
    CRO_STATIC_ANONYMOUS = 0x120,
    CRO_STATIC_ANONYMOUS_COUNT = 0x124,
    CRO_INTERNAL = 0x128,
    CRO_INTERNAL_COUNT = 0x12c,
    CRO_STATIC = 0x130,
    CRO_STATIC_COUNT = 0x134
};
/* Where cro_image puts the tables and the segments in the file */
enum {
    IMG_CODE = 0x1000,
    IMG_RODATA = 0x2000,
    IMG_NAME = IMG_RODATA,
    IMG_SEGMENTS = 0x2010,
    IMG_EXPORT_NAMED = 0x2050,
    IMG_EXPORT_TREE = 0x2058,
    IMG_EXPORT_STRINGS = 0x2060,
    IMG_IMPORT_MODULES = 0x2080,
    IMG_EXTERNAL = 0x2100,
    IMG_IMPORT_NAMED = 0x2110,
    IMG_IMPORT_STRINGS = 0x2120,
    IMG_STATIC_ANONYMOUS = 0x2130,
    IMG_INTERNAL = 0x2140,
    IMG_STATIC = 0x2150,
    IMG_DATA = 0x3000
};
#define IMG_SEGMENT(index) (IMG_SEGMENTS + SEGMENT_RECORD * (index))
/* The fields of a record, from the start of the record */
enum {
    SEGMENT_BASE = 0,
    SEGMENT_SIZE = 4,
    SEGMENT_TYPE = 8,
    RELOCATION_TAG = 0,
    RELOCATION_TYPE = 4,
    RELOCATION_END = 5,     /* of a batch, in the external and static tables */
    RELOCATION_SEGMENT = 5, /* of the symbol, in the internal table */
    RELOCATION_STATE = 6,   /* of the first record of a batch: 1 once the batch is applied */
    RELOCATION_ADDEND = 8,
    EXPORT_NAME = 0,
    EXPORT_TAG = 4,
    IMPORT_NAME = 0,
    IMPORT_BATCH = 4,
    IMPORT_MODULE_INDEXED = 4,
    IMPORT_MODULE_INDEXED_COUNT = 8,
    IMPORT_MODULE_ANONYMOUS = 12,
    IMPORT_MODULE_ANONYMOUS_COUNT = 16
};
/* The words of a LoadCRO request, in the order of its arguments after the command header */
enum {
    LOAD_BUFFER,
    LOAD_ADDRESS,
    LOAD_SIZE,
    LOAD_DATA,
    LOAD_RESERVED,
    LOAD_DATA_SIZE,
    LOAD_BSS,
    LOAD_BSS_SIZE,
    LOAD_AUTO_LINK,
    LOAD_FIX,
    LOAD_SPARE,
    LOAD_WORDS
};
/* the request of a module that cro_image makes, with the one CRS import linked into .data */
#define CRO_LOAD_DEFAULT                                                                                               \
    {[LOAD_BUFFER] = CRO_SOURCE, [LOAD_ADDRESS] = CRO_MODULE, [LOAD_SIZE] = CRO_IMAGE_SIZE, [LOAD_DATA] = CRO_DATA,    \
     [LOAD_DATA_SIZE] = 0x100,   [LOAD_BSS] = CRO_BSS,        [LOAD_BSS_SIZE] = 0x100,      [LOAD_AUTO_LINK] = 1}
/* The symbol and the segments of the image that cro_image(.., true) makes, after the loader mapped it as the CRS */
#define CRS_SEGMENT(index) (0x100000u + 0x100000u * (index))
#define CRS_SYMBOL         (CRS_SEGMENT(0) + CRO_ENTRY)

/* store a little-endian word at a byte offset of an image */
void cro_set(unsigned char* image, uint32_t at, uint32_t value);
/* A CRO0 image of CRO_IMAGE_SIZE bytes with four segments and either one named CRS export or one .data import */
void cro_image(unsigned char* image, bool crs);
/* One static anonymous relocation of a module made by cro_image(.., false). It writes the address of the module's
   code segment plus 0x44 and the addend at the tag, in the code of the static module. */
void cro_add_static_relocation(unsigned char* image, uint32_t tag, uint32_t addend);
/* ControlMemory operations and the heap the tests map for the buffers: size bytes at CRO_SOURCE */
enum { MEMOP_FREE = 1, MEMOP_ALLOC = 3, MEMOP_PROTECT = 6 };
void cro_map_heap(SnxCtrProcess* p, uint32_t size);
/* the guest's command buffer, which the SVC reads the request from and writes the reply to */
uint32_t cro_command_buffer(SnxCtrProcess* p);
/* An ldr:ro command with n normal words and the process handle. It returns the transport result, or the service's
   result word, and the size the loader kept for LoadCRO when fixed is not NULL. */
uint32_t cro_call(SnxCtrProcess* p, uint32_t session, unsigned command, const uint32_t* args, unsigned n,
                  uint32_t handle, uint32_t* fixed);
/* a session to ldr:ro, through srv: */
uint32_t cro_service(SnxCtrProcess* p);
#endif
