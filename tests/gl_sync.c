/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The shared GL layer's sync objects (src/common/gpu/gl/gl_sync.c), with no GPU and a platform whose serials the test
   moves: a fence of the work recorded, unsignaled until the platform finished it, waited for with a timeout of 0 and
   with one (the platform's wait asked for its serial), its parameters, GL's errors, a platform with no work (every
   fence signaled), and a save state's copy loaded back with its fences signaled and refused when its size is wrong */
#include "common/gpu/gl/gl_state.h"
#include "common/gpu/gl/gl_sync.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    GPU_COMMANDS_COMPLETE = 0x9117,
    STATUS = 0x9114,
    SIGNALED = 0x9119,
    UNSIGNALED = 0x9118,
    ALREADY_SIGNALED = 0x911a,
    TIMEOUT_EXPIRED = 0x911b,
    CONDITION_SATISFIED = 0x911c,
    WAIT_FAILED = 0x911d,
};

/* the platform: the work recorded and done, and the waits asked */
static struct {
    uint64_t recorded, completed, waited;
} gpu;
static uint64_t recorded(void) { return gpu.recorded; }
static uint64_t completed(void) { return gpu.completed; }
static void wait_for(uint32_t ctx, uint64_t serial) {
    (void)ctx;
    gpu.waited = serial;
    gpu.completed = serial;
}
static int32_t parameter(uint32_t ctx, uint64_t sync, uint32_t pname) {
    int32_t v = -1;
    assert(snx_gl_get_sync(ctx, sync, pname, &v));
    return v;
}

int main(void) {
    uint32_t ctx = snx_gl_context_new(SNX_GL_API_DESKTOP, 45, 0);
    uint64_t a, b;
    assert(ctx);
    snx_gl_sync_host(&(SnxGlSyncHost){recorded, completed, wait_for});

    /* a fence of the work recorded: unsignaled until the platform finished it */
    gpu.recorded = 5, gpu.completed = 4;
    a = snx_gl_fence_sync(ctx, GPU_COMMANDS_COMPLETE, 0);
    assert(a && snx_gl_is_sync(a) && !snx_gl_is_sync(a + 1) && parameter(ctx, a, STATUS) == UNSIGNALED);
    assert(parameter(ctx, a, 0x9112) == 0x9116 && parameter(ctx, a, 0x9113) == GPU_COMMANDS_COMPLETE);
    assert(snx_gl_client_wait_sync(ctx, a, 0, 0) == TIMEOUT_EXPIRED && !gpu.waited);
    assert(snx_gl_client_wait_sync(ctx, a, 1, 1000) == CONDITION_SATISFIED && gpu.waited == 5);
    assert(parameter(ctx, a, STATUS) == SIGNALED && snx_gl_client_wait_sync(ctx, a, 0, 0) == ALREADY_SIGNALED);
    /* GL's errors */
    assert(!snx_gl_fence_sync(ctx, 0x1234, 0) && snx_gl_get_error(ctx) == SNX_GL_INVALID_ENUM);
    assert(!snx_gl_fence_sync(ctx, GPU_COMMANDS_COMPLETE, 1) && snx_gl_get_error(ctx) == SNX_GL_INVALID_VALUE);
    assert(snx_gl_client_wait_sync(ctx, 999, 0, 0) == WAIT_FAILED && snx_gl_get_error(ctx) == SNX_GL_INVALID_VALUE);
    assert(snx_gl_client_wait_sync(ctx, a, 2, 0) == WAIT_FAILED && snx_gl_get_error(ctx) == SNX_GL_INVALID_VALUE);
    snx_gl_wait_sync(ctx, a, 0, 0xffffffffffffffffull);
    assert(!snx_gl_get_error(ctx));
    snx_gl_wait_sync(ctx, a, 0, 5);
    assert(snx_gl_get_error(ctx) == SNX_GL_INVALID_VALUE);
    {
        int32_t v = 0;
        assert(!snx_gl_get_sync(ctx, a, 0x1234, &v) && snx_gl_get_error(ctx) == SNX_GL_INVALID_ENUM);
    }
    /* a save state's copy: its fences signaled after the load, the work before it done */
    gpu.recorded = 9;
    b = snx_gl_fence_sync(ctx, GPU_COMMANDS_COMPLETE, 0);
    assert(parameter(ctx, b, STATUS) == UNSIGNALED);
    {
        size_t total = snx_gl_state_bytes();
        unsigned char* saved = malloc(total);
        char why[200];
        assert(saved);
        snx_gl_state_save(saved);
        assert(!snx_gl_state_check(saved, total, why, sizeof why) && !snx_gl_state_load(saved, total));
        assert(snx_gl_is_sync(b) && parameter(ctx, b, STATUS) == SIGNALED);
        assert(snx_gl_state_check(saved, total - 8, why, sizeof why) < 0 && strstr(why, "this build reads"));
        free(saved);
    }
    /* deletes: the name freed, one that is none refused, 0 taken */
    snx_gl_delete_sync(ctx, a);
    assert(!snx_gl_is_sync(a) && !snx_gl_get_error(ctx));
    snx_gl_delete_sync(ctx, a);
    assert(snx_gl_get_error(ctx) == SNX_GL_INVALID_VALUE);
    snx_gl_delete_sync(ctx, 0);
    assert(!snx_gl_get_error(ctx));
    /* a platform with no work: every fence signaled */
    snx_gl_sync_host(NULL);
    a = snx_gl_fence_sync(ctx, GPU_COMMANDS_COMPLETE, 0);
    assert(snx_gl_client_wait_sync(ctx, a, 0, 0) == ALREADY_SIGNALED);
    puts("gl_sync: ok");
    return 0;
}
