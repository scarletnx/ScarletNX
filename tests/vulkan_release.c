/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Deterministic queue scheduling: real release/writeback code, fake completed GPU
   submissions. No Vulkan device or worker timing is needed to expose recursion,
   decreasing gates, physical aliases, and remapping order. */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "nx/gpu/gpu.h"
#include "nx/runtime/platform/plat.h"
#define SNX_VK_H
#define VK_NULL_HANDLE 0
void vk_result_begin(void);
void vk_result_end(void);
int vk_result_landing(void);
void vk_lock(void);
void vk_unlock(void);
void vk_compute_land_through(u64);
void vk_land_through(u64);
void vk_wait_submit(u64);
void vk_compute_wait(u64);
void vk_gfx_image_land(void);
u64 vk_compute_put_aside(void);
int vk_buf_pending_any(void);
int vk_buf_pending_unsubmitted(void);
void vk_submit(u64, u64, u64);
u64 vk_last_submit(void);
void vk_rt_before_write(u64, u64);
#include "nx/gpu/vk/release.c"
#include "nx/gpu/vk/writeback.c"

int g_gpu_releases;
static unsigned lock_depth, landing_calls, payload_writes;
static u64 next_compute, next_graphics = 1, waited_compute, waited_graphics;
static int pending, landing_probe;
static u32 completed[128];
static u8 memory[8192];
static u64 mapping = 4096;
void pm_lock(PMutex* m) { (void)m; }
void pm_unlock(PMutex* m) { (void)m; }
void pc_init(PCond* c) { (void)c; }
void pc_broadcast(PCond* c) { (void)c; }
int pc_wait(PCond* c, PMutex* m, i64 n) {
    (void)c;
    (void)m;
    (void)n;
    abort();
}
int plat_thread(PThreadFn fn, void* arg, size_t stack) {
    (void)fn;
    (void)arg;
    (void)stack;
    return 0;
}
void fatal(const char* fmt, ...) {
    (void)fmt;
    abort();
}
void vk_lock(void) { lock_depth++; }
void vk_unlock(void) {
    assert(lock_depth);
    lock_depth--;
}
u64 gmmu_translate(u64 address) {
    if (address >= 0x10000 && address < 0x11000) return mapping + (address & 4095);
    if (address >= 0x20000 && address < 0x21000) return 4096 + (address & 4095);
    return 0;
}
u32 syncpt_read(u32 id) { return id < 128 ? completed[id] : 0; }
void syncpt_incr(u32 id) {
    assert(lock_depth && vk_result_landing());
    completed[id]++;
}
int gmem_write(u64 address, const void* data, u64 size) {
    assert(lock_depth && vk_result_landing());
    vk_release_need(address, size); /* the real memory layer reenters here */
    u64 physical = gmmu_translate(address);
    assert(physical && physical + size <= sizeof memory);
    memcpy(memory + physical, data, size);
    payload_writes++;
    return 1;
}
u8* gmem_ptr(u64 address, u64 size) {
    (void)address;
    (void)size;
    return NULL;
}
int gmem_read(u64 address, void* data, u64 size) {
    vk_release_need(address, size);
    memcpy(data, memory + gmmu_translate(address), size);
    return 1;
}
void gpu_mem_written_shader(u64 address, u64 size) {
    (void)address;
    (void)size;
}
void vk_rt_before_write(u64 address, u64 size) {
    (void)address;
    (void)size;
}
void vk_compute_land_through(u64 gate) {
    assert(lock_depth && vk_result_landing());
    assert(waited_compute >= gate);
    landing_calls++;
    if (landing_probe) {
        landing_probe = 0;
        u8 original = 0, result = 7;
        vk_write_changed(0x10000, &result, &original, 1);
        assert(completed[3] == 0); /* release follows all output reconciliation */
    }
}
void vk_land_through(u64 gate) { assert(waited_graphics >= gate); }
void vk_wait_submit(u64 gate) {
    if (gate > waited_graphics) waited_graphics = gate;
}
void vk_compute_wait(u64 gate) {
    if (gate > waited_compute) waited_compute = gate;
}
void vk_gfx_image_land(void) {}
u64 vk_compute_put_aside(void) {
    u64 value = next_compute;
    next_compute = 0;
    return value;
}
int vk_buf_pending_any(void) { return pending; }
int vk_buf_pending_unsubmitted(void) { return 0; }
void vk_submit(u64 a, u64 b, u64 c) {
    (void)a;
    (void)b;
    (void)c;
}
u64 vk_last_submit(void) { return next_graphics; }

int main(void) {
    pending = 1;
    next_compute = 9;
    vk_release_incr(3);
    next_compute = 0; /* older compute work has landed through another read */
    u8 value = 11;
    vk_release_write(0x10000, &value, 1);
    assert(g_rq[1].cgate == 9 && vk_release_value(3) == 1);
    landing_probe = 1;
    vk_release_need(0x20000, 1); /* another GVA names the same backing */
    assert(landing_calls == 1 && completed[3] == 1 && memory[4096] == 11);
    assert(g_rhead == g_rtail && !g_gpu_releases && !vk_result_landing());
    assert(vk_release_value(3) == 1);
    value = 13;
    vk_release_write(0x10000, &value, 1);
    vk_release_drain(); /* mapping callback must drain before changing backing */
    mapping = 1;
    assert(memory[4096] == 13 && memory[1] == 0);
    pending = 0;
    value = 17;
    vk_release_write(0x10000, &value, 1); /* immediate writes hold the mapping lock */
    assert(memory[1] == 17 && payload_writes == 4);
    pending = 1;
    vk_release_incr(5);
    vk_release_retire(5);
    assert(completed[5] == 1 && g_rhead == g_rtail);
    assert(lock_depth == 0);
    return 0;
}
