/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "3ds/aot/ctr_module.h"
#include "3ds/kernel/ctr_kernel.h"
#include "3ds/runtime/internal.h"
#include "common/utilities/common.h"
#include "support/3ds_guest.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define INVALID_HANDLE 0xd8e007f7u
#define INVALID_ENUM   0xd8e093edu
#define TIMEOUT        0x09401bfeu
static uint32_t used(SnxCtrProcess* p) {
    p->cpu.state.cpu.r[1] = 0;
    p->cpu.state.cpu.r[2] = 1;
    assert(snx_ctr_svc(p, 0x2a) == SNX_CTR_SVC_RETURN && !p->cpu.state.cpu.r[0]);
    assert(!p->cpu.state.cpu.r[2]);
    return p->cpu.state.cpu.r[1];
}
static uint32_t memory(SnxCtrProcess* p, uint32_t op, uint32_t va, uint32_t size, uint32_t permissions,
                       uint32_t expected) {
    uint32_t* r = p->cpu.state.cpu.r;
    r[0] = op;
    r[1] = va;
    r[2] = 0;
    r[3] = size;
    r[4] = permissions;
    assert(snx_ctr_svc(p, 1) == SNX_CTR_SVC_RETURN && r[0] == expected);
    return r[1];
}
static uint32_t create(SnxCtrProcess* p) {
    assert(snx_ctr_svc(p, 0x21) == SNX_CTR_SVC_RETURN && !p->cpu.state.cpu.r[0]);
    return p->cpu.state.cpu.r[1];
}
static void close_handle(SnxCtrProcess* p, uint32_t handle, uint32_t expected) {
    p->cpu.state.cpu.r[0] = handle;
    assert(snx_ctr_svc(p, 0x23) == SNX_CTR_SVC_RETURN && p->cpu.state.cpu.r[0] == expected);
}
static void arbitrate(SnxCtrProcess* p, uint32_t handle, uint32_t va, uint32_t type, uint32_t value, uint64_t timeout,
                      SnxCtrSvcResult state, uint32_t result) {
    uint32_t* r = p->cpu.state.cpu.r;
    r[0] = handle;
    r[1] = va;
    r[2] = type;
    r[3] = value;
    r[4] = (uint32_t)timeout;
    r[5] = (uint32_t)(timeout >> 32);
    assert(snx_ctr_svc(p, 0x22) == state);
    if (state == SNX_CTR_SVC_RETURN) assert(r[0] == result);
}
/* an SVC taking r0..r3, answering r0 and the 64-bit value at r1:r2 */
static uint64_t info(SnxCtrProcess* p, uint32_t number, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3,
                     uint32_t expected) {
    uint32_t* r = p->cpu.state.cpu.r;
    r[0] = r0;
    r[1] = r1;
    r[2] = r2;
    r[3] = r3;
    assert(snx_ctr_svc(p, number) == SNX_CTR_SVC_RETURN && r[0] == expected);
    return (uint64_t)r[2] << 32 | r[1];
}
/* GetSystemInfo, GetProcessInfo, GetThreadInfo, GetHandleInfo, the resource limit, CreateThread's processors and
   priorities, the cache and Luma SVCs, Break (Azahar svc.cpp, resource_limit.cpp) */
static void information(SnxCtrProcess* p, unsigned model) {
    const uint32_t enum_kernel = 0xd8e007edu;
    uint32_t* r = p->cpu.state.cpu.r;
    uint64_t used = info(p, 0x2a, 0, 0, 1, 0, 0);
    assert(used && used == info(p, 0x2b, 0, CTR_PROCESS_HANDLE, 0, 0, 0));
    assert(info(p, 0x2a, 0, 0x10000, 0x80, 0, 0) == p->app_limit);
    assert(info(p, 0x2a, 0, 0x10000, 0x201, 0, 0) == model);
    assert(info(p, 0x2a, 0, 26, 0, 0, 0) == 5 && !info(p, 0x2a, 0, 0x20000, 0, 0, 0));
    info(p, 0x2a, 0, 0x10001, 0, 0, model ? 0 : enum_kernel);
    assert(info(p, 0x2b, 0, CTR_PROCESS_HANDLE, 20, 0, 0) == 0x0c000000);
    assert(info(p, 0x2b, 0, CTR_PROCESS_HANDLE, 0x10001, 0, 0) == UINT64_C(0x0004000000998200));
    assert(info(p, 0x2b, 0, CTR_PROCESS_HANDLE, 0x10000, 0, 0) == 0x74736574u); /* "test", little-endian */
    assert(info(p, 0x2b, 0, CTR_PROCESS_HANDLE, 0x10002, 0, 0) == 4096);
    assert(info(p, 0x2b, 0, CTR_PROCESS_HANDLE, 0x10004, 0, 0) == 8192); /* one data page and one of BSS */
    assert(info(p, 0x2b, 0, CTR_PROCESS_HANDLE, 0x10006, 0, 0) == 0x200000);
    info(p, 0x2b, 0, CTR_PROCESS_HANDLE, 21, 0, 0xe0e01bf4u);
    info(p, 0x2b, 0, CTR_PROCESS_HANDLE, 99, 0, enum_kernel);
    info(p, 0x2b, 0, 0, 0, 0, INVALID_HANDLE);
    assert(info(p, 0x2c, 0, CTR_THREAD_HANDLE, 0x10000, 0, 0) == SNX_CTR_INITIAL_TLS);
    info(p, 0x2c, 0, CTR_THREAD_HANDLE, 0, 0, enum_kernel);
    info(p, 0x36, 0, CTR_THREAD_HANDLE, 0, 0, 0);
    assert(r[1] == CTR_PROCESS_ID);
    /* the resource limit: one handle per call to the same object, the APPLICATION values */
    info(p, 0x38, 0, CTR_PROCESS_HANDLE, 0, 0, 0);
    uint32_t limit = r[1], id = snx_ctr_handle_object(p, limit);
    assert(id && p->objects[id].type == CTR_OBJ_RESOURCE_LIMIT);
    assert(info(p, 0x29, 0, limit, 1, 0, 0) == 1); /* references less one: the process's and this handle's */
    const uint32_t names[] = {0, 1, 2, 6};
    for (unsigned i = 0; i < 4; ++i) ctr_put(p, 0x300100 + i * 4, names[i]);
    info(p, 0x39, 0x300200, limit, 0x300100, 4, 0);
    assert(ctr_word(p, 0x300200) == 0x18 && ctr_word(p, 0x300208) == p->app_limit && ctr_word(p, 0x300210) == 0x20);
    assert(ctr_word(p, 0x300218) == 8 && !ctr_word(p, 0x30021c));
    info(p, 0x3a, 0x300200, limit, 0x300100, 4, 0);
    assert(ctr_word(p, 0x300208) == used && ctr_word(p, 0x300210) == 1 &&
           !ctr_word(p, 0x300218)); /* one thread, no timer */
    ctr_put(p, 0x300100, 10);
    info(p, 0x39, 0x300200, limit, 0x300100, 1, enum_kernel);
    close_handle(p, limit, 0);
    /* CreateThread: processors -2 to 3, no better priority than the limit's */
    for (int32_t processor = -2; processor <= 3; ++processor) {
        r[4] = (uint32_t)processor;
        info(p, 8, 0x30, 0x100004, 0, SNX_CTR_STACK_TOP, 0);
        uint32_t tid = snx_ctr_handle_object(p, r[1]);
        assert(tid && p->objects[tid].data.thread.context.fpscr == 0x03c00000u); /* DN, FZ, round to zero */
        p->objects[tid].data.thread.dead = 1; /* out of the way of the scheduling below */
        close_handle(p, r[1], 0);
        snx_ctr_object_release(p, tid);
    }
    r[4] = 4;
    info(p, 8, 0x30, 0x100004, 0, SNX_CTR_STACK_TOP, 0xe0e01bfdu);
    r[4] = 0;
    info(p, 8, 0x17, 0x100004, 0, SNX_CTR_STACK_TOP, 0xd9001beau);
    info(p, 8, 64, 0x100004, 0, SNX_CTR_STACK_TOP, 0xe0e01bfdu);
    /* cache maintenance and Luma's instruction cache SVCs succeed; Break stops the run */
    for (uint32_t number = 0x52; number <= 0x54; ++number) {
        info(p, number, CTR_PROCESS_HANDLE, 0x300000, 4096, 0, 0);
        info(p, number, 0, 0x300000, 4096, 0, INVALID_HANDLE);
    }
    info(p, 0x93, 0x100000, 4096, 0, 0, 0);
    info(p, 0x94, 0, 0, 0, 0, 0);
    r[0] = 1;
    assert(snx_ctr_svc(p, 0x3c) == SNX_CTR_SVC_UNSUPPORTED);
}
static uint32_t control(SnxCtrProcess* p, uint32_t op, uint32_t va, uint32_t source, uint32_t size,
                        uint32_t permissions) {
    uint32_t* r = p->cpu.state.cpu.r;
    r[0] = op;
    r[1] = va;
    r[2] = source;
    r[3] = size;
    r[4] = permissions;
    assert(snx_ctr_svc(p, 1) == SNX_CTR_SVC_RETURN);
    return r[0];
}
/* ControlMemory MAP/UNMAP and the memory block SVCs (Azahar svc.cpp, process.cpp, shared_memory.cpp) */
static void memory_blocks(SnxCtrProcess* p) {
    const uint32_t state = 0xe0a01bf5u, address = 0xe0e01bf5u, combination = 0xe0e01beeu, dont_care = 0x10000000u;
    uint32_t* r = p->cpu.state.cpu.r;
    assert(!control(p, 3, 0x08000000, 0, 0x2000, 3));
    ctr_put(p, 0x08000000, 0x12345678);
    /* MAP mirrors the heap pages; while it lasts neither side can be freed */
    assert(!control(p, 4, 0x0c000000, 0x08000000, 0x2000, 3));
    assert(ctr_word(p, 0x0c000000) == 0x12345678);
    ctr_put(p, 0x0c001000, 0xcafe);
    assert(ctr_word(p, 0x08001000) == 0xcafe);
    assert(control(p, 1, 0x08000000, 0, 0x1000, 0) == state);
    assert(control(p, 4, 0x0d000000, 0x08000000, 0x1000, 3) == state);       /* the source is aliased already */
    assert(control(p, 4, 0x08001000, 0x08000000, 0x2000, 3) == state);       /* overlapping */
    assert(control(p, 4, 0x0d000000, 0x14000000, 0x1000, 3) == address);     /* not heap */
    assert(control(p, 5, 0x0c000000, 0x08000000, 0x1000, 3) == 0xd8e007f5u); /* not the pair MAP made */
    assert(!control(p, 5, 0x0c000000, 0x08000000, 0x2000, 3));
    assert(snx_ctr_query(p->space, 0x0c000000, NULL, NULL) != SNX_CTR_MEM_OK && ctr_word(p, 0x08000000) == 0x12345678);
    /* a block over the application's own heap: Locked while it lives, mapped elsewhere as a mirror */
    r[0] = 0;
    r[1] = 0x08000000;
    r[2] = 0x1000;
    r[3] = 3;
    assert(snx_ctr_svc(p, 0x1e) == SNX_CTR_SVC_RETURN && !r[0]);
    uint32_t block = r[1], id = snx_ctr_handle_object(p, block);
    assert(id && p->objects[id].data.shared.address == 0x08000000);
    assert(control(p, 1, 0x08000000, 0, 0x1000, 0) == state);
    r[0] = block;
    r[1] = 0x0d000000;
    r[2] = 1;
    r[3] = 0; /* another process's rights must include the block's own */
    assert(snx_ctr_svc(p, 0x1f) == SNX_CTR_SVC_RETURN && r[0] == 0xd900182eu);
    r[0] = block;
    r[1] = 0x0d000000;
    r[2] = 1;
    r[3] = 3;
    assert(snx_ctr_svc(p, 0x1f) == SNX_CTR_SVC_RETURN && !r[0] && ctr_word(p, 0x0d000000) == 0x12345678);
    assert(control(p, 1, 0x0d000000, 0, 0x1000, 0) == state); /* the mirror is not plain private heap either */
    assert(snx_ctr_check_ram(p->space, 0x0d000000, 4, SNX_CTR_WRITE) != SNX_CTR_MEM_OK); /* mapped read-only */
    r[0] = block;
    r[1] = 0x0d000000;
    r[2] = 3;
    r[3] = dont_care; /* not for a heap-backed block */
    assert(snx_ctr_svc(p, 0x1f) == SNX_CTR_SVC_RETURN && r[0] == combination);
    r[0] = block;
    r[1] = 0x0e000000;
    assert(snx_ctr_svc(p, 0x20) == SNX_CTR_SVC_RETURN && r[0] == state); /* never mapped there */
    r[0] = block;
    r[1] = 0x0d000000;
    assert(snx_ctr_svc(p, 0x20) == SNX_CTR_SVC_RETURN && !r[0]);
    assert(snx_ctr_query(p->space, 0x0d000000, NULL, NULL) != SNX_CTR_MEM_OK);
    close_handle(p, block, 0);
    assert(!control(p, 1, 0x08000000, 0, 0x2000, 0)); /* free again once the block is gone */
    /* address 0 maps a block with pages where they lie in the old linear heap, and a block with none has no address */
    uint32_t pages = snx_ctr_shared_pages(p, 0x1000);
    assert(pages && !snx_ctr_object_create(p, CTR_OBJ_SHARED, &block));
    uint32_t pages_id = snx_ctr_handle_object(p, block);
    p->objects[pages_id].data.shared.size = 0x1000;
    p->objects[pages_id].data.shared.permissions = SNX_CTR_READ;
    r[0] = block;
    r[1] = 0;
    r[2] = SNX_CTR_READ;
    r[3] = 0;
    assert(snx_ctr_svc(p, 0x1f) == SNX_CTR_SVC_RETURN && r[0] == address);
    p->objects[pages_id].data.shared.physical = pages;
    r[0] = block;
    r[1] = 0;
    r[2] = SNX_CTR_READ;
    r[3] = 0;
    uint32_t linear = CTR_LINEAR + (pages - CTR_PHYS_BASE), mapped_pa = 0;
    assert(snx_ctr_svc(p, 0x1f) == SNX_CTR_SVC_RETURN && !r[0]);
    assert(snx_ctr_query(p->space, linear, &mapped_pa, NULL) == SNX_CTR_MEM_OK && mapped_pa == pages);
    r[0] = block;
    r[1] = linear;
    assert(snx_ctr_svc(p, 0x20) == SNX_CTR_SVC_RETURN && !r[0]);
    close_handle(p, block, 0);
    /* an application may not ask the kernel for a new block (address 0): 0xD92007EA (3dbrew) */
    r[0] = dont_care;
    r[1] = 0;
    r[2] = 0x1000;
    r[3] = 3;
    assert(snx_ctr_svc(p, 0x1e) == SNX_CTR_SVC_RETURN && r[0] == 0xd92007eau && !r[1]);
    /* refusals: a size not in pages, an execute permission, a range that is not private heap, an unaligned one */
    r[0] = 0;
    r[1] = 0;
    r[2] = 0x800;
    r[3] = 3;
    assert(snx_ctr_svc(p, 0x1e) == SNX_CTR_SVC_RETURN && r[0] == 0xe0e01bf2u);
    r[2] = 0x1000;
    r[3] = 4;
    assert(snx_ctr_svc(p, 0x1e) == SNX_CTR_SVC_RETURN && r[0] == combination);
    r[0] = 0;
    r[1] = 0x09000000;
    r[3] = 3;
    assert(snx_ctr_svc(p, 0x1e) == SNX_CTR_SVC_RETURN && r[0] == state);
    r[0] = 0;
    r[1] = 0x08000800;
    assert(snx_ctr_svc(p, 0x1e) == SNX_CTR_SVC_RETURN && r[0] == 0xe0e01bf1u);
}
int main(void) {
    unsigned char code[0x3000] = {0};
    SnxCtrImage image = {.segment = {{0x100000, 1, 16}, {0x200000, 1, 4}, {0x300000, 1, 4}},
                         .stack_size = 0x4000,
                         .main_priority = 0x30,
                         .bss_size = 0x1000,
                         .title_id = UINT64_C(0x0004000000998200),
                         .name = {'t', 'e', 's', 't'},
                         .code = code,
                         .code_size = sizeof code};
    for (unsigned model = 0; model < 2; ++model) {
        SnxCtrProcess* q = snx_ctr_process_new(&image, (SnxCtrModel)model);
        assert(q);
        information(q, model);
        snx_ctr_process_free(q);
        q = snx_ctr_process_new(&image, (SnxCtrModel)model);
        assert(q);
        memory_blocks(q);
        snx_ctr_process_free(q);
        SnxCtrProcess* p = snx_ctr_process_new(&image, (SnxCtrModel)model);
        assert(p);
        assert(snx_ctr_check_ram(p->space, 0x1ff81000, 4096, SNX_CTR_READ) == SNX_CTR_MEM_OK);
        assert(snx_ctr_check_ram(p->space, 0x1ff81000, 1, SNX_CTR_WRITE) == SNX_CTR_MEM_PERMISSION);
        /* the first time entry went where the counter now points (index counter & 1), as Azahar writes it */
        uint32_t counter = ctr_word(p, 0x1ff81000), entry = 0x1ff81020 + (counter & 1) * 0x20;
        uint64_t utc = (uint64_t)ctr_word(p, entry) | (uint64_t)ctr_word(p, entry + 4) << 32;
        assert(counter == 1 && utc >= UINT64_C(2208988800000) && ctr_word(p, entry + 0x10) == 268111856);
        assert((ctr_word(p, 0x1ff81004) & 0xff) == 1 && (ctr_word(p, 0x1ff81084) >> 8 & 0xffff) == 0x0117);
        assert((ctr_word(p, 0x1ff81064) >> 16 & 0xffff) == 0x0203); /* Wi-Fi: best link, Internet */
        p->time_update_ns = 0; /* the hourly entry, due now: the other entry, then the counter */
        assert(snx_ctr_schedule(p) && ctr_word(p, 0x1ff81000) == 2 && ctr_word(p, 0x1ff81020 + 0x10) == 268111856);
        assert(snx_ctr_svc(p, 0x28) == SNX_CTR_SVC_RETURN);
        uint64_t ticks = (uint64_t)p->cpu.state.cpu.r[0] | (uint64_t)p->cpu.state.cpu.r[1] << 32;
        assert(snx_ctr_svc(p, 0x28) == SNX_CTR_SVC_RETURN);
        assert(((uint64_t)p->cpu.state.cpu.r[0] | (uint64_t)p->cpu.state.cpu.r[1] << 32) >= ticks);
        uint32_t* r = p->cpu.state.cpu.r;
        assert(!snx_ctr_kernel_blocked(p));
        unsigned permissions;
        assert(snx_ctr_query(p->space, CTR_CONFIG, NULL, &permissions) == SNX_CTR_MEM_OK && permissions == 1);
        assert(ctr_word(p, CTR_CONFIG) == 0x023a0000);
        assert(ctr_word(p, CTR_CONFIG + 0x40) == p->app_limit);
        assert((uint64_t)ctr_word(p, CTR_CONFIG + 0x40) + ctr_word(p, CTR_CONFIG + 0x44) +
                   ctr_word(p, CTR_CONFIG + 0x48) ==
               snx_ctr_profile((SnxCtrModel)model)->fcram_size);
        assert(snx_ctr_write_ram(p->space, CTR_CONFIG, "x", 1) == SNX_CTR_MEM_PERMISSION);
        assert(snx_ctr_fetch(p->space, CTR_CONFIG, code, 4) == SNX_CTR_MEM_PERMISSION);
        uint32_t baseline = used(p);
        assert(baseline == 0x8000); /* the code and the stack: the TLS page is the BASE region's, outside the pool */
        uint32_t va = memory(p, 3, CTR_HEAP, 8192, 3, 0);
        assert(va == CTR_HEAP && used(p) == baseline + 8192);
        assert(!ctr_word(p, va) && !ctr_word(p, va + 4096));
        ctr_put(p, va, 0xabcdef01);
        assert(memory(p, 6, va, 4096, 1, 0) == va);
        assert(snx_ctr_write_ram(p->space, va, "x", 1) == SNX_CTR_MEM_PERMISSION);
        memory(p, 6, va, 4096, 3, 0);
        uint64_t epoch = snx_ctr_memory_epoch(p->space);
        memory(p, 1, va + 4096, 8192, 0, 0xe0a01bf5u);
        assert(snx_ctr_memory_epoch(p->space) == epoch && used(p) == baseline + 8192 && ctr_word(p, va) == 0xabcdef01);
        memory(p, 1, va, 4096, 0, 0);
        assert(used(p) == baseline + 4096 && snx_ctr_query(p->space, va, NULL, NULL) == SNX_CTR_MEM_UNMAPPED);
        assert(memory(p, 3, va, 4096, 3, 0) == va && !ctr_word(p, va)); /* recycled RAM is zero */
        memory(p, 1, va, 8192, 0, 0);
        assert(used(p) == baseline);
        /* A normal heap allocation can join physically discontiguous holes. */
        memory(p, 3, CTR_HEAP, 3 * 4096, 3, 0);
        memory(p, 1, CTR_HEAP + 4096, 4096, 0, 0);
        uint32_t scatter = CTR_HEAP + 0x10000, first_pa, next_pa;
        memory(p, 3, scatter, 8192, 3, 0);
        assert(snx_ctr_query(p->space, scatter, &first_pa, NULL) == SNX_CTR_MEM_OK);
        assert(snx_ctr_query(p->space, scatter + 4096, &next_pa, NULL) == SNX_CTR_MEM_OK && next_pa != first_pa + 4096);
        assert(snx_ctr_write_ram(p->space, scatter + 4092, "12345678", 8) == SNX_CTR_MEM_OK);
        memory(p, 6, scatter + 4096, 4096, 1, 0);
        assert(snx_ctr_write_ram(p->space, scatter + 4092, "abcdefgh", 8) == SNX_CTR_MEM_PERMISSION);
        assert(ctr_word(p, scatter + 4092) == 0x34333231 && ctr_word(p, scatter + 4096) == 0x38373635);
        memory(p, 1, scatter, 8192, 0, 0);
        memory(p, 1, CTR_HEAP, 4096, 0, 0);
        memory(p, 1, CTR_HEAP + 8192, 4096, 0, 0);
        assert(used(p) == baseline);
        memory(p, 3, 0, 4096, 3, 0xe0e01bf5u);
        memory(p, 3, CTR_HEAP + 1, 4096, 3, 0xe0e01bf1u);
        memory(p, 3, CTR_HEAP, 4097, 3, 0xe0e01bf2u);
        memory(p, 3, CTR_HEAP, 4096, 4, 0xe0e01beeu);
        memory(p, 3, 0x100000, 4096, 3, 0xe0e01bf5u);
        memory(p, 3, 0xfffff000, 8192, 3, 0xe0e01bf5u);
        memory(p, 3, 0, 0xfffff000, 3, 0xe0e01bfdu);
        memory(p, 0x20003, 0, 4096, 3, 0xe0e01beeu);
        memory(p, 4, CTR_HEAP, 4096, 3, 0xe0e01bf5u); /* MAP from address 0: not heap (memory_blocks has the rest) */
        memory(p, 3, CTR_HEAP, p->app_limit, 3, 0xc8601bf3u);
        memory(p, 1, 0x100000, 4096, 0, 0xe0e01bf5u);
        assert(used(p) == baseline);
        uint32_t linear = memory(p, 0x10003, 0, 8192, 3, 0), pa;
        assert(snx_ctr_query(p->space, linear, &pa, NULL) == SNX_CTR_MEM_OK);
        assert(linear - CTR_LINEAR == pa - CTR_PHYS_BASE && !ctr_word(p, linear));
        memory(p, 0x10003, linear, 4096, 3, 0xe0a01bf5u);
        memory(p, 0x10003, CTR_LINEAR, 4096, 3, 0xe0a01bf5u); /* static physical pages cannot be aliased */
        memory(p, 1, linear, 8192, 0, 0);
        assert(used(p) == baseline);
        r[0] = 3;
        r[1] = CTR_HEAP;
        r[2] = 1;
        r[3] = 4096;
        r[4] = 3;
        assert(snx_ctr_svc(p, 1) == SNX_CTR_SVC_RETURN && r[0] == 0xe0e01bf1u);
        assert(used(p) == baseline);
        r[1] = 2; /* the kernel's allocated pages: 0 with success, as Azahar answers */
        r[2] = 0;
        assert(snx_ctr_svc(p, 0x2a) == SNX_CTR_SVC_RETURN && !r[0] && !r[1] && !r[2]);
        /* GetThreadId (0x37) answers in r1 and is not the debug string (0x3d) */
        r[1] = CTR_THREAD_HANDLE;
        assert(snx_ctr_svc(p, 0x37) == SNX_CTR_SVC_RETURN && !r[0] && r[1] == p->current_thread && r[1]);
        r[1] = 0;
        assert(snx_ctr_svc(p, 0x37) == SNX_CTR_SVC_RETURN && r[0] == 0xd8e007f7u);
        uint32_t handle = create(p);
        assert(handle && ctr_object_count(p) == 3);
        r[1] = handle;
        assert(snx_ctr_svc(p, 0x27) == SNX_CTR_SVC_RETURN && !r[0]);
        uint32_t duplicate = r[1];
        assert(duplicate != handle && ctr_object_count(p) == 3);
        close_handle(p, handle, 0);
        close_handle(p, handle, INVALID_HANDLE);
        arbitrate(p, handle, 0x300000, 0, UINT32_MAX, 0, SNX_CTR_SVC_RETURN, INVALID_HANDLE);
        arbitrate(p, duplicate, 0x300000, 0, UINT32_MAX, 0, SNX_CTR_SVC_RETURN, 0);
        uint32_t next = create(p);
        assert(next != handle);
        close_handle(p, next, 0);
        close_handle(p, CTR_THREAD_HANDLE, INVALID_HANDLE);
        close_handle(p, CTR_PROCESS_HANDLE, INVALID_HANDLE);
        r[1] = CTR_THREAD_HANDLE;
        assert(snx_ctr_svc(p, 0x27) == SNX_CTR_SVC_RETURN && !r[0]);
        uint32_t thread = r[1];
        arbitrate(p, thread, 0x300000, 0, 1, 0, SNX_CTR_SVC_RETURN, INVALID_HANDLE);
        close_handle(p, thread, 0);
        arbitrate(p, duplicate, 0x300001, 1, 0, 0, SNX_CTR_SVC_RETURN, 0xe0e01bf1u);
        arbitrate(p, duplicate, 0x300000, 5, 0, 0, SNX_CTR_SVC_RETURN, 0xd8e093edu);
        arbitrate(p, duplicate, 0x400000, 1, 0, 0, SNX_CTR_SVC_RETURN, 0xe0e01bf5u);
        ctr_put(p, 0x300000, 5);
        arbitrate(p, duplicate, 0x300000, 2, 3, 0, SNX_CTR_SVC_RETURN, 0);
        assert(ctr_word(p, 0x300000) == 5);
        arbitrate(p, duplicate, 0x300000, 3, 3, 0, SNX_CTR_SVC_RETURN, TIMEOUT);
        arbitrate(p, duplicate, 0x300000, 4, 6, 0, SNX_CTR_SVC_RETURN, TIMEOUT);
        assert(ctr_word(p, 0x300000) == 4);
        ctr_put(p, 0x300000, UINT32_MAX);
        arbitrate(p, duplicate, 0x300000, 4, 0, 0, SNX_CTR_SVC_RETURN, TIMEOUT);
        assert(ctr_word(p, 0x300000) == UINT32_MAX - 1);
        /* a finite timed wait decrements, blocks, and times out at its deadline (the only thread: the scheduler
           sleeps the host until then), its reference on the arbiter released */
        uint32_t arbiter_refs = p->objects[snx_ctr_handle_object(p, duplicate)].refs;
        arbitrate(p, duplicate, 0x300000, 4, 0, 100000, SNX_CTR_SVC_BLOCKED, 0);
        assert(ctr_word(p, 0x300000) == UINT32_MAX - 2 && snx_ctr_kernel_blocked(p));
        assert(snx_ctr_schedule(p) && !snx_ctr_kernel_blocked(p) && r[0] == TIMEOUT);
        assert(p->objects[snx_ctr_handle_object(p, duplicate)].refs == arbiter_refs);
        close_handle(p, duplicate, 0);
        assert(ctr_object_count(p) == 2);
        uint32_t handles[CTR_HANDLES];
        for (unsigned i = 0; i < CTR_HANDLES; ++i) {
            r[1] = CTR_THREAD_HANDLE;
            assert(snx_ctr_svc(p, 0x27) == SNX_CTR_SVC_RETURN && !r[0]);
            handles[i] = r[1];
        }
        r[1] = CTR_THREAD_HANDLE;
        assert(snx_ctr_svc(p, 0x27) == SNX_CTR_SVC_RETURN && r[0] == 0xd8600413u && !r[1]);
        assert(snx_ctr_svc(p, 0x21) == SNX_CTR_SVC_RETURN && r[0] == 0xd8600413u && !r[1] && ctr_object_count(p) == 2);
        for (unsigned i = 0; i < CTR_HANDLES; ++i) close_handle(p, handles[i], 0);
        /* the application may make two arbiters (its resource limit), then OutOfAddressArbiters */
        uint32_t arbiters[2];
        for (unsigned i = 0; i < 2; ++i) arbiters[i] = create(p);
        assert(snx_ctr_svc(p, 0x21) == SNX_CTR_SVC_RETURN && r[0] == 0xc8601833u && !r[1]);
        for (unsigned i = 0; i < 2; ++i) close_handle(p, arbiters[i], 0);
        /* the object table itself runs out after all but the process, the main thread and the null slot */
        unsigned count = 0;
        uint32_t made = 0;
        while (!snx_ctr_object_create(p, CTR_OBJ_EVENT, &made)) {
            assert(count < CTR_HANDLES);
            handles[count++] = made;
        }
        assert(count == CTR_OBJECTS - 3 && !made && ctr_object_count(p) == CTR_OBJECTS - 1);
        for (unsigned i = 0; i < count; ++i) close_handle(p, handles[i], 0);
        assert(ctr_object_count(p) == 2);
        /* thread priorities and mutexes over the real object model */
        r[1] = 1;
        assert(snx_ctr_svc(p, 0x13) == SNX_CTR_SVC_RETURN && !r[0]);
        uint32_t mutex = r[1];
        assert(mutex && ctr_object_count(p) == 3);
        r[0] = mutex;
        assert(snx_ctr_svc(p, 0x14) == SNX_CTR_SVC_RETURN && !r[0]); /* the creator owns it */
        r[0] = mutex;
        assert(snx_ctr_svc(p, 0x14) == SNX_CTR_SVC_RETURN && r[0] == 0xd8e0041fu);
        close_handle(p, mutex, 0);
        r[1] = 2; /* any nonzero value locks it, as a bool */
        assert(snx_ctr_svc(p, 0x13) == SNX_CTR_SVC_RETURN && !r[0] && r[1]);
        mutex = r[1];
        assert(p->objects[snx_ctr_handle_object(p, mutex)].data.mutex.depth == 1);
        close_handle(p, mutex, 0);
        r[0] = 0x30;
        r[1] = 0x100004;
        r[2] = 0;
        r[3] = SNX_CTR_STACK_TOP - 0x1000;
        r[4] = 0xfffffffeu;
        assert(snx_ctr_svc(p, 0x08) == SNX_CTR_SVC_RETURN && !r[0]);
        uint32_t worker = r[1];
        r[1] = worker;
        assert(snx_ctr_svc(p, 0x0b) == SNX_CTR_SVC_RETURN && !r[0] && r[1] == 0x30);
        r[0] = worker;
        r[1] = 0x28;
        assert(snx_ctr_svc(p, 0x0c) == SNX_CTR_SVC_RETURN && !r[0]);
        r[1] = worker;
        assert(snx_ctr_svc(p, 0x0b) == SNX_CTR_SVC_RETURN && r[1] == 0x28);
        r[0] = worker;
        r[1] = 0x80;
        assert(snx_ctr_svc(p, 0x0c) == SNX_CTR_SVC_RETURN && r[0] == 0xe0e01bfdu); /* out of range */
        r[0] = worker;
        r[1] = 0x10;
        assert(snx_ctr_svc(p, 0x0c) == SNX_CTR_SVC_RETURN && r[0] == 0xd9001beau); /* better than the limit */
        p->objects[snx_ctr_handle_object(p, worker)].data.thread.dead = 1;         /* this test never runs it */
        close_handle(p, worker, 0);
        /* semaphores: consume, timed-out empty wait, release, bounded counts */
        r[1] = 1;
        r[2] = 2;
        assert(snx_ctr_svc(p, 0x15) == SNX_CTR_SVC_RETURN && !r[0]);
        uint32_t sem = r[1];
        r[0] = sem;
        r[2] = 0;
        r[3] = 0;
        assert(snx_ctr_svc(p, 0x24) == SNX_CTR_SVC_RETURN && !r[0]); /* consumes the one count */
        r[0] = sem;
        r[2] = 0;
        r[3] = 0;
        assert(snx_ctr_svc(p, 0x24) == SNX_CTR_SVC_RETURN && r[0] == TIMEOUT); /* empty: immediate wait times out */
        r[1] = sem;
        r[2] = 2;
        assert(snx_ctr_svc(p, 0x16) == SNX_CTR_SVC_RETURN && !r[0] && r[1] == 0);
        r[1] = sem;
        r[2] = 1;
        assert(snx_ctr_svc(p, 0x16) == SNX_CTR_SVC_RETURN && r[0] == 0xd8e007fdu && r[1] == 0); /* past the maximum */
        r[1] = sem;
        r[2] = 0;
        assert(snx_ctr_svc(p, 0x16) == SNX_CTR_SVC_RETURN && !r[0] && r[1] == 2); /* releasing none changes nothing */
        close_handle(p, sem, 0);
        /* SleepThread: zero yields, a delay blocks the thread until its deadline (the only thread here, so the
           scheduler sleeps the host for it), -1 sleeps forever: nothing is left to wake for */
        r[0] = 0;
        r[1] = 0;
        assert(snx_ctr_svc(p, 0x0a) == SNX_CTR_SVC_RETURN);
        r[0] = 1000000;
        r[1] = 0;
        double slept = snx_seconds();
        assert(snx_ctr_svc(p, 0x0a) == SNX_CTR_SVC_BLOCKED && snx_ctr_thread(p)->wake_ns);
        assert(snx_ctr_schedule(p) && !snx_ctr_thread(p)->wake_ns && snx_seconds() - slept >= 0.001);
        r[0] = 0xffffffff;
        r[1] = 0xffffffffu;
        assert(snx_ctr_svc(p, 0x0a) == SNX_CTR_SVC_BLOCKED && !snx_ctr_schedule(p));
        snx_ctr_thread(p)->wake_ns = 0;
        r[0] = 0xdeadbeef;
        assert(snx_ctr_svc(p, 0x7f) == SNX_CTR_SVC_UNSUPPORTED && r[0] == 0xdeadbeef);
        handle = create(p);
        ctr_put(p, 0x300000, 0);
        arbitrate(p, handle, 0x300000, 2, 1, 0, SNX_CTR_SVC_BLOCKED, 0);
        assert(snx_ctr_kernel_blocked(p) && ctr_word(p, 0x300000) == UINT32_MAX);
        assert(snx_ctr_svc(p, 0x21) == SNX_CTR_SVC_BLOCKED);
        SnxCtrModule module = {0};
        uint64_t executed = 99;
        uint32_t pc = r[15];
        assert(snx_ctr_dispatch(&module, p, 10, &executed) == SNX_CTR_RUN_BLOCKED && !executed && r[15] == pc);
        snx_ctr_process_free(p);
    }
    puts(
        "CTR kernel objects, stale handles, resource exhaustion, checked memory and truthful arbitration/waits passed");
    return 0;
}
