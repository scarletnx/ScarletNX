/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "3ds_cro.h"
#include "common/utilities/endian.h"

void cro_set(unsigned char* image, uint32_t at, uint32_t value) { snx_st_le32(image + at, value); }
void cro_image(unsigned char* image, bool crs) {
    memset(image, 0, CRO_IMAGE_SIZE);
    cro_set(image, CRO_HEADER, CRO_MAGIC);
    cro_set(image, CRO_FILE_SIZE, CRO_IMAGE_SIZE);
    cro_set(image, CRO_BSS_SIZE, 0x100);
    cro_set(image, CRO_ON_UNRESOLVED, CRO_TAG(0, CRO_UNRESOLVED));
    cro_set(image, CRO_CODE_OFFSET, IMG_CODE);
    cro_set(image, CRO_CODE_SIZE, 0x1000);
    cro_set(image, CRO_DATA_OFFSET, IMG_DATA);
    cro_set(image, CRO_DATA_SIZE, 0x100);
    const char* name = crs ? "|static|" : "module";
    memcpy(image + IMG_NAME, name, strlen(name) + 1);
    cro_set(image, CRO_MODULE_NAME, IMG_NAME);
    cro_set(image, CRO_MODULE_NAME_SIZE, crs ? 9 : 8);
    cro_set(image, CRO_SEGMENTS, IMG_SEGMENTS);
    cro_set(image, CRO_SEGMENT_COUNT, 4);
    const uint32_t offset[] = {IMG_CODE, IMG_RODATA, IMG_DATA, 0};
    const uint32_t size[] = {0x1000, 0x1000, 0x100, 0x100};
    for (uint32_t i = 0; i < 4; ++i) {
        /* a CRS holds absolute addresses, and its bss lies elsewhere than the rest */
        cro_set(image, IMG_SEGMENT(i) + SEGMENT_BASE, crs ? (i == 3 ? CRS_BSS : CRS_SEGMENT(i)) : offset[i]);
        cro_set(image, IMG_SEGMENT(i) + SEGMENT_SIZE, size[i]);
        cro_set(image, IMG_SEGMENT(i) + SEGMENT_TYPE, i);
    }
    /* every table starts empty at the external relocations */
    for (uint32_t at = CRO_EXPORT_NAMED; at <= CRO_STATIC; at += 8) cro_set(image, at, IMG_EXTERNAL);
    cro_set(image, CRO_EXPORT_NAMED, IMG_EXPORT_NAMED);
    cro_set(image, CRO_EXPORT_NAMED_COUNT, crs ? 1 : 0);
    cro_set(image, CRO_EXPORT_INDEXED, IMG_EXPORT_STRINGS);
    cro_set(image, CRO_EXPORT_STRINGS, IMG_EXPORT_STRINGS);
    cro_set(image, CRO_EXPORT_STRINGS_SIZE, 6);
    memcpy(image + IMG_EXPORT_STRINGS, "entry", 6);
    cro_set(image, CRO_EXPORT_TREE, IMG_EXPORT_TREE);
    cro_set(image, CRO_IMPORT_MODULES, IMG_IMPORT_MODULES);
    cro_set(image, CRO_EXTERNAL, IMG_EXTERNAL);
    cro_set(image, CRO_EXTERNAL_COUNT, crs ? 0 : 1);
    cro_set(image, CRO_IMPORT_NAMED, IMG_IMPORT_NAMED);
    cro_set(image, CRO_IMPORT_NAMED_COUNT, crs ? 0 : 1);
    cro_set(image, CRO_IMPORT_INDEXED, IMG_IMPORT_STRINGS);
    cro_set(image, CRO_IMPORT_ANONYMOUS, IMG_IMPORT_STRINGS);
    cro_set(image, CRO_IMPORT_STRINGS, IMG_IMPORT_STRINGS);
    cro_set(image, CRO_IMPORT_STRINGS_SIZE, 6);
    memcpy(image + IMG_IMPORT_STRINGS, "entry", 6);
    cro_set(image, CRO_STATIC_ANONYMOUS, IMG_STATIC_ANONYMOUS);
    cro_set(image, CRO_INTERNAL, IMG_INTERNAL);
    cro_set(image, CRO_STATIC, IMG_STATIC);
    /* the export "entry" is 0x44 bytes into the code */
    cro_set(image, IMG_EXPORT_NAMED + EXPORT_NAME, IMG_EXPORT_STRINGS);
    cro_set(image, IMG_EXPORT_NAMED + EXPORT_TAG, CRO_TAG(0, CRO_ENTRY));
    /* one external relocation, which writes the symbol at the start of .data, and the import that names it */
    cro_set(image, IMG_EXTERNAL + RELOCATION_TAG, CRO_TAG(2, 0));
    image[IMG_EXTERNAL + RELOCATION_TYPE] = 2;
    image[IMG_EXTERNAL + RELOCATION_END] = 1;
    cro_set(image, IMG_IMPORT_NAMED + IMPORT_NAME, IMG_IMPORT_STRINGS);
    cro_set(image, IMG_IMPORT_NAMED + IMPORT_BATCH, IMG_EXTERNAL);
    cro_set(image, IMG_CODE, 0xe3a00001u);     /* mov r0, #1 */
    cro_set(image, IMG_CODE + 4, 0xeafffffeu); /* b . */
}
void cro_add_static_relocation(unsigned char* image, uint32_t tag, uint32_t addend) {
    cro_set(image, CRO_STATIC_ANONYMOUS_COUNT, 1);
    cro_set(image, CRO_STATIC_COUNT, 1);
    cro_set(image, IMG_STATIC_ANONYMOUS + EXPORT_NAME, CRO_TAG(0, CRO_ENTRY));
    cro_set(image, IMG_STATIC_ANONYMOUS + IMPORT_BATCH, IMG_STATIC);
    cro_set(image, IMG_STATIC + RELOCATION_TAG, tag);
    image[IMG_STATIC + RELOCATION_TYPE] = 2;
    image[IMG_STATIC + RELOCATION_END] = 1;
    cro_set(image, IMG_STATIC + RELOCATION_ADDEND, addend);
}
void cro_map_heap(SnxCtrProcess* p, uint32_t size) {
    uint32_t out;
    assert(!snx_ctr_kernel_memory(p, MEMOP_ALLOC, CRO_SOURCE, 0, size, SNX_CTR_READ | SNX_CTR_WRITE, &out));
}
uint32_t cro_command_buffer(SnxCtrProcess* p) { return p->cpu.state.cpu.tpidruro + 0x80; }
uint32_t cro_call(SnxCtrProcess* p, uint32_t session, unsigned command, const uint32_t* args, unsigned n,
                  uint32_t handle, uint32_t* fixed) {
    uint32_t cmd = cro_command_buffer(p);
    unsigned char zero[256] = {0};
    assert(snx_ctr_write_ram(p->space, cmd, zero, sizeof zero) == SNX_CTR_MEM_OK);
    ctr_put(p, cmd, IPC_HEADER(command, n, 2));
    for (unsigned i = 0; i < n; ++i) ctr_put(p, cmd + (i + 1) * 4, args[i]);
    ctr_put(p, cmd + (n + 1) * 4, handle == CRO_PID_DESCRIPTOR ? IPC_PROCESS_ID : 0);
    ctr_put(p, cmd + (n + 2) * 4, handle);
    p->cpu.state.cpu.r[0] = session;
    assert(snx_ctr_svc(p, SVC_SEND_SYNC_REQUEST) == SNX_CTR_SVC_RETURN);
    uint32_t transport = p->cpu.state.cpu.r[0];
    if (transport) return transport;
    /* the reply has two normal words for the commands that load a CRO, and one for the others */
    assert(ctr_word(p, cmd) == IPC_HEADER(command, command == 4 || command == 9 ? 2 : 1, 0));
    if (fixed) *fixed = ctr_word(p, cmd + 8);
    return ctr_word(p, cmd + 4);
}
uint32_t cro_service(SnxCtrProcess* p) {
    const char name[] = "srv:";
    assert(snx_ctr_write_ram(p->space, CRO_SOURCE, name, sizeof name) == SNX_CTR_MEM_OK);
    p->cpu.state.cpu.r[1] = CRO_SOURCE;
    assert(snx_ctr_svc(p, SVC_CONNECT_TO_PORT) == SNX_CTR_SVC_RETURN && !p->cpu.state.cpu.r[0]);
    uint32_t srv = p->cpu.state.cpu.r[1];
    uint32_t cmd = cro_command_buffer(p);
    unsigned char zero[256] = {0};
    assert(snx_ctr_write_ram(p->space, cmd, zero, sizeof zero) == SNX_CTR_MEM_OK);
    ctr_put(p, cmd, IPC_HEADER(1, 0, 2)); /* RegisterClient, with a process id descriptor */
    ctr_put(p, cmd + 4, IPC_PROCESS_ID);
    p->cpu.state.cpu.r[0] = srv;
    assert(snx_ctr_svc(p, SVC_SEND_SYNC_REQUEST) == SNX_CTR_SVC_RETURN && !p->cpu.state.cpu.r[0] &&
           !ctr_word(p, cmd + 4));
    ctr_put(p, cmd, IPC_HEADER(5, 4, 0)); /* GetServiceHandle: the name "ldr:ro" in two words, its length and flags */
    const char ldr_name[8] = "ldr:ro";
    assert(snx_ctr_write_ram(p->space, cmd + 4, ldr_name, sizeof ldr_name) == SNX_CTR_MEM_OK);
    ctr_put(p, cmd + 12, 6);
    ctr_put(p, cmd + 16, 1);
    p->cpu.state.cpu.r[0] = srv;
    assert(snx_ctr_svc(p, SVC_SEND_SYNC_REQUEST) == SNX_CTR_SVC_RETURN && !p->cpu.state.cpu.r[0]);
    assert(ctr_word(p, cmd) == IPC_HEADER(5, 1, 2) && !ctr_word(p, cmd + 4) && ctr_word(p, cmd + 8) == IPC_MOVE_HANDLE);
    uint32_t ldr = ctr_word(p, cmd + 12);
    assert(!snx_ctr_handle_close(p, srv));
    return ldr;
}
