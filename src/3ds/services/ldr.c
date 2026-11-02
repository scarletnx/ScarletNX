/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Port of Azahar service/ldr_ro/ldr_ro.cpp at a4b335e.
   Copyright 2014 Citra Emulator Project
   Licensed under GPLv2 or any later version. */
#include "3ds/services/ldr.h"
#include "3ds/services/cro.h"
#include "3ds/runtime/internal.h"
#include "common/utilities/bits.h"
#include <stdlib.h>
#include <string.h>

#define RO_NOT_INITIALIZED     0xd9612ff8u
#define RO_ALREADY_INITIALIZED 0xd9612ff9u
#define RO_INVALID_IMAGE       0xd8601803u
#define RO_NOT_LOADED          0xd8a12c0du
#define RO_INVALID_MEMORY      0xd8a12c08u
#define RO_INVALID_HANDLE      0xd8e007f7u
#define RO_READ_WRITE          (SNX_CTR_READ | SNX_CTR_WRITE)
/* the words of a LoadCRO request, after its command header, in the order ldr:ro reads them */
enum {
    IN_BUFFER = 1,
    IN_ADDRESS,
    IN_SIZE,
    IN_DATA,
    IN_RESERVED,
    IN_DATA_SIZE,
    IN_BSS,
    IN_BSS_SIZE,
    IN_AUTO_LINK,
    IN_FIX
};
bool snx_ctr_ldr_held(SnxCtrProcess* p, uint32_t address, uint32_t size) {
    if (snx_ranges_overlap(address, size, p->ldr_crs, p->ldr_crs_size) ||
        snx_ranges_overlap(address, size, p->ldr_buffer, p->ldr_crs_size))
        return true;
    for (size_t i = 0; i < p->ldr_count; ++i) {
        SnxCtrLdrModule* m = &p->ldr_modules[i];
        if (snx_ranges_overlap(address, size, m->address, m->size) ||
            snx_ranges_overlap(address, size, m->buffer, m->size) ||
            snx_ranges_overlap(address, size, m->data, m->data_size) ||
            snx_ranges_overlap(address, size, m->bss, m->bss_size))
            return true;
    }
    return false;
}
static SnxCtrLdrModule* find_module(SnxCtrProcess* p, uint32_t address) {
    for (size_t i = 0; i < p->ldr_count; ++i)
        if (p->ldr_modules[i].address == address) return &p->ldr_modules[i];
    return NULL;
}
static bool reserve_module(SnxCtrProcess* p) {
    if (p->ldr_count < p->ldr_capacity) return true;
    size_t capacity = p->ldr_capacity ? p->ldr_capacity * 2 : 8;
    if (capacity < p->ldr_capacity || capacity > SIZE_MAX / sizeof *p->ldr_modules) return false;
    SnxCtrLdrModule* modules = realloc(p->ldr_modules, capacity * sizeof *modules);
    if (!modules) return false;
    p->ldr_modules = modules;
    p->ldr_capacity = capacity;
    return true;
}
static void reply(SnxCtrIpc* c, uint32_t result, uint32_t fixed) {
    uint32_t command = c->in[0] >> 16;
    bool loads = command == 4 || command == 9;
    c->out[0] = command << 16 | (loads ? 2u : 1u) << 6;
    c->out[1] = result;
    if (loads) c->out[2] = result ? 0 : fixed;
}
static bool process_handle(SnxCtrIpc* c, uint32_t normal) {
    if (!snx_ctr_ipc_expect(c, normal, 2)) return false;
    if (c->in[normal + 1] || c->handle_count != 1 || c->handle_moved) {
        snx_ctr_ipc_bad(c);
        return false;
    }
    if (snx_ctr_handle_object(c->process, c->handle_in[0]) != snx_ctr_handle_object(c->process, CTR_PROCESS_HANDLE)) {
        reply(c, RO_INVALID_HANDLE, 0);
        return false;
    }
    return true;
}
static bool copy_data(SnxCtrProcess* p, uint32_t to, uint32_t from, uint32_t size) {
    uint8_t bytes[4096];
    for (uint32_t off = 0; off < size;) {
        uint32_t n = snx_min_u32(size - off, sizeof bytes);
        if (snx_ctr_read_ram(p->space, from + off, bytes, n) != SNX_CTR_MEM_OK ||
            !snx_ctr_cro_write(p, to + off, bytes, n))
            return false;
        off += n;
    }
    return true;
}
static bool clear_bss(SnxCtrProcess* p, uint32_t at, uint32_t size) {
    uint8_t zero[4096] = {0};
    for (uint32_t off = 0; off < size;) {
        uint32_t n = snx_min_u32(size - off, sizeof zero);
        if (!snx_ctr_cro_write(p, at + off, zero, n)) return false;
        off += n;
    }
    return true;
}
static void initialize(SnxCtrIpc* c) {
    SnxCtrProcess* p = c->process;
    if (!process_handle(c, 3)) return;
    if (p->ldr_crs) {
        reply(c, RO_ALREADY_INITIALIZED, 0);
        return;
    }
    uint32_t buffer = c->in[1], size = c->in[2], address = c->in[3];
    if (!snx_ctr_cro_validate(p, buffer, size, true)) {
        reply(c, RO_INVALID_IMAGE, 0);
        return;
    }
    if (!snx_ctr_kernel_loader_map(p, address, buffer, size)) {
        reply(c, RO_INVALID_MEMORY, 0);
        return;
    }
    if (!snx_ctr_cro_begin(p, address, size, buffer)) {
        (void)snx_ctr_unmap(p->space, address, size);
        reply(c, RO_INVALID_MEMORY, 0);
        return;
    }
    bool result = snx_ctr_cro_rebase_crs(p, address, size);
    result = result && snx_ctr_protect(p->space, address, size, SNX_CTR_READ) == SNX_CTR_MEM_OK;
    result = result && snx_ctr_protect(p->space, buffer, size, SNX_CTR_READ) == SNX_CTR_MEM_OK;
    result = snx_ctr_cro_finish(p, result);
    if (!result) {
        (void)snx_ctr_protect(p->space, buffer, size, RO_READ_WRITE);
        (void)snx_ctr_unmap(p->space, address, size);
        reply(c, RO_INVALID_IMAGE, 0);
        return;
    }
    p->ldr_crs = address;
    p->ldr_crs_size = size;
    p->ldr_buffer = buffer;
    reply(c, 0, 0);
}
static void load_cro(SnxCtrIpc* c, bool new_command) {
    SnxCtrProcess* p = c->process;
    if (!process_handle(c, 11)) return;
    if (!p->ldr_crs) {
        reply(c, RO_NOT_INITIALIZED, 0);
        return;
    }
    uint32_t buffer = c->in[IN_BUFFER], address = c->in[IN_ADDRESS], size = c->in[IN_SIZE], data = c->in[IN_DATA],
             data_size = c->in[IN_DATA_SIZE], bss = c->in[IN_BSS], bss_size = c->in[IN_BSS_SIZE], fix = c->in[IN_FIX];
    if (c->in[IN_RESERVED] || c->in[IN_AUTO_LINK] > 1 || fix > 3 || !snx_ctr_cro_validate(p, buffer, size, false)) {
        reply(c, RO_INVALID_IMAGE, 0);
        return;
    }
    uint32_t data_offset, data_bytes;
    snx_ctr_cro_data(p, buffer, &data_offset, &data_bytes);
    if (data_bytes > data_size || !snx_ctr_kernel_private(p, data, data_size) ||
        !snx_ctr_kernel_private(p, bss, bss_size) || snx_ranges_overlap(data, data_size, bss, bss_size) ||
        snx_ranges_overlap(buffer, size, data, data_size) || snx_ranges_overlap(buffer, size, bss, bss_size) ||
        snx_ranges_overlap(address, size, data, data_size) || snx_ranges_overlap(address, size, bss, bss_size) ||
        !reserve_module(p)) {
        reply(c, RO_INVALID_MEMORY, 0);
        return;
    }
    if (!snx_ctr_kernel_loader_map(p, address, buffer, size)) {
        reply(c, RO_INVALID_MEMORY, 0);
        return;
    }
    if (!snx_ctr_cro_begin(p, address, size, buffer)) {
        (void)snx_ctr_unmap(p->space, address, size);
        reply(c, RO_INVALID_MEMORY, 0);
        return;
    }
    bool result = clear_bss(p, bss, bss_size) &&
                  snx_ctr_cro_rebase(p, address, size, data, data_size, bss, bss_size, p->ldr_crs) &&
                  snx_ctr_cro_link_load(p, address, p->ldr_crs, new_command) &&
                  copy_data(p, data, address + data_offset, data_bytes);
    uint32_t fixed = 0, code = 0, code_size = 0;
    if (result) {
        snx_ctr_cro_register(p, address, p->ldr_crs, c->in[IN_AUTO_LINK] != 0);
        fixed = snx_ctr_cro_fix(p, address, fix);
        result = fixed && fixed <= size;
        result = result && snx_ctr_protect(p->space, address, fixed, SNX_CTR_READ) == SNX_CTR_MEM_OK;
        result = result && snx_ctr_cro_code(p, address, &code, &code_size);
        if (result && code_size)
            result = snx_ctr_protect(p->space, code, code_size, SNX_CTR_READ | SNX_CTR_EXEC) == SNX_CTR_MEM_OK;
        result = result && snx_ctr_protect(p->space, buffer, fixed, SNX_CTR_READ) == SNX_CTR_MEM_OK;
        result = result && snx_ctr_cro_good(p);
        /* The returned fixed size releases the tail to the caller. It must not stay held by the loader. */
        if (result && fixed < size) result = snx_ctr_unmap(p->space, address + fixed, size - fixed) == SNX_CTR_MEM_OK;
    }
    if (!result) (void)snx_ctr_protect(p->space, buffer, size, RO_READ_WRITE);
    result = snx_ctr_cro_finish(p, result);
    if (!result) {
        (void)snx_ctr_unmap(p->space, address, size);
        reply(c, RO_INVALID_IMAGE, 0);
        return;
    }
    p->ldr_modules[p->ldr_count++] = (SnxCtrLdrModule){address, buffer, fixed, data, data_size, bss, bss_size};
    reply(c, 0, fixed);
}
static void unload_cro(SnxCtrIpc* c) {
    SnxCtrProcess* p = c->process;
    if (!process_handle(c, 3)) return;
    SnxCtrLdrModule* m = find_module(p, c->in[1]);
    if (!p->ldr_crs || !m || c->in[2] || m->buffer != c->in[3]) {
        reply(c, RO_NOT_LOADED, 0);
        return;
    }
    if (!snx_ctr_cro_begin(p, m->address, m->size, m->buffer)) {
        reply(c, RO_INVALID_MEMORY, 0);
        return;
    }
    snx_ctr_cro_unregister(p, m->address, p->ldr_crs);
    bool result = snx_ctr_cro_unlink(p, m->address, p->ldr_crs) && snx_ctr_cro_unrebase(p, m->address, false);
    /* Keep every page mapped until all journaled writes succeed. A failed unmap changes no mappings. */
    result = result && snx_ctr_cro_good(p);
    result = result && snx_ctr_protect(p->space, m->buffer, m->size, RO_READ_WRITE) == SNX_CTR_MEM_OK;
    result = result && snx_ctr_unmap(p->space, m->address, m->size) == SNX_CTR_MEM_OK;
    if (!result) (void)snx_ctr_protect(p->space, m->buffer, m->size, SNX_CTR_READ);
    if (!snx_ctr_cro_finish(p, result)) {
        reply(c, RO_INVALID_IMAGE, 0);
        return;
    }
    size_t index = (size_t)(m - p->ldr_modules);
    memmove(m, m + 1, (p->ldr_count - index - 1) * sizeof *m);
    --p->ldr_count;
    reply(c, 0, 0);
}
static void link_cro(SnxCtrIpc* c, bool link) {
    SnxCtrProcess* p = c->process;
    if (!process_handle(c, 1)) return;
    SnxCtrLdrModule* m = find_module(p, c->in[1]);
    if (!p->ldr_crs || !m) {
        reply(c, RO_NOT_LOADED, 0);
        return;
    }
    if (!snx_ctr_cro_begin(p, m->address, m->size, m->buffer)) {
        reply(c, RO_INVALID_MEMORY, 0);
        return;
    }
    bool result = link ? snx_ctr_cro_link(p, m->address, p->ldr_crs) : snx_ctr_cro_unlink(p, m->address, p->ldr_crs);
    reply(c, snx_ctr_cro_finish(p, result) ? 0 : RO_INVALID_IMAGE, 0);
}
static void ro_shutdown(SnxCtrIpc* c) {
    SnxCtrProcess* p = c->process;
    if (!process_handle(c, 1)) return;
    if (!p->ldr_crs) {
        reply(c, RO_NOT_INITIALIZED, 0);
        return;
    }
    if (p->ldr_count || c->in[1] != p->ldr_buffer) {
        reply(c, RO_INVALID_MEMORY, 0);
        return;
    }
    if (!snx_ctr_cro_begin(p, p->ldr_crs, p->ldr_crs_size, p->ldr_buffer)) {
        reply(c, RO_INVALID_MEMORY, 0);
        return;
    }
    bool result = snx_ctr_cro_unrebase(p, p->ldr_crs, true);
    result = result && snx_ctr_cro_good(p);
    result = result && snx_ctr_protect(p->space, p->ldr_buffer, p->ldr_crs_size, RO_READ_WRITE) == SNX_CTR_MEM_OK;
    result = result && snx_ctr_unmap(p->space, p->ldr_crs, p->ldr_crs_size) == SNX_CTR_MEM_OK;
    if (!result) (void)snx_ctr_protect(p->space, p->ldr_buffer, p->ldr_crs_size, SNX_CTR_READ);
    if (!snx_ctr_cro_finish(p, result)) {
        reply(c, RO_INVALID_IMAGE, 0);
        return;
    }
    p->ldr_crs = 0;
    p->ldr_crs_size = 0;
    p->ldr_buffer = 0;
    reply(c, 0, 0);
}
void snx_ctr_ldr_dispatch(SnxCtrIpc* c) {
    uint32_t command = c->in[0] >> 16;
    switch (command) {
        case 1: initialize(c); break;
        case 2:
        case 3:
            /* CRR hash verification is intentionally unsupported, as in the reference HLE. */
            if (process_handle(c, command == 2 ? 2 : 1)) reply(c, 0, 0);
            break;
        case 4:
        case 9: load_cro(c, command == 9); break;
        case 5: unload_cro(c); break;
        case 6:
        case 7: link_cro(c, command == 6); break;
        case 8: ro_shutdown(c); break;
        default: c->state = SNX_CTR_IPC_UNKNOWN; break;
    }
}
