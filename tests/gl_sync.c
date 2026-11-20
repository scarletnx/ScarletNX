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

/* the platform: the work recorded and done and the waits asked, and the lines the layer said and its last */
static struct {
    struct {
        uint64_t recorded, completed, waited;
    } gpu;
    int said;
    char last_said[200];
} g_gl_sync_test;
static uint64_t recorded(void) { return g_gl_sync_test.gpu.recorded; }
static uint64_t completed(void) { return g_gl_sync_test.gpu.completed; }
static void wait_for(uint32_t ctx, uint64_t serial) {
    (void)ctx;
    g_gl_sync_test.gpu.waited = serial;
    g_gl_sync_test.gpu.completed = serial;
}
static int32_t parameter(uint32_t ctx, uint64_t sync, uint32_t pname) {
    int32_t v = -1;
    assert(snx_gl_get_sync(ctx, sync, pname, &v));
    return v;
}

static void told(const char* line) {
    g_gl_sync_test.said++;
    snprintf(g_gl_sync_test.last_said, sizeof g_gl_sync_test.last_said, "%s", line);
}

int main(void) {
    uint32_t ctx = snx_gl_context_new(SNX_GL_API_DESKTOP, 45, 0);
    uint64_t a, b;
    assert(ctx);
    snx_gl_sync_host(&(SnxGlSyncHost){recorded, completed, wait_for, told});

    /* a fence of the work recorded: unsignaled until the platform finished it */
    g_gl_sync_test.gpu.recorded = 5, g_gl_sync_test.gpu.completed = 4;
    a = snx_gl_fence_sync(ctx, GPU_COMMANDS_COMPLETE, 0);
    assert(a && snx_gl_is_sync(a) && !snx_gl_is_sync(a + 1) && parameter(ctx, a, STATUS) == UNSIGNALED);
    assert(parameter(ctx, a, 0x9112) == 0x9116 && parameter(ctx, a, 0x9113) == GPU_COMMANDS_COMPLETE);
    assert(snx_gl_client_wait_sync(ctx, a, 0, 0) == TIMEOUT_EXPIRED && !g_gl_sync_test.gpu.waited);
    assert(snx_gl_client_wait_sync(ctx, a, 1, 1000) == CONDITION_SATISFIED && g_gl_sync_test.gpu.waited == 5);
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
    g_gl_sync_test.gpu.recorded = 9;
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
    /* a deleted fence's handle names no sync though its record is used again: a game that tests a handle it kept past
       the delete with glIsSync finds it deleted, not the next fence's */
    {
        uint64_t next = snx_gl_fence_sync(ctx, GPU_COMMANDS_COMPLETE, 0), loaded;
        size_t total = snx_gl_state_bytes();
        unsigned char* saved = malloc(total);
        assert(next && next != a && (uint32_t)next == (uint32_t)a && !snx_gl_is_sync(a) && snx_gl_is_sync(next));
        snx_gl_delete_sync(ctx, a);
        assert(snx_gl_get_error(ctx) == SNX_GL_INVALID_VALUE && snx_gl_is_sync(next));
        /* after a load, the ids go on past the saved ones */
        assert(saved);
        snx_gl_state_save(saved);
        assert(!snx_gl_state_load(saved, total));
        free(saved);
        loaded = snx_gl_fence_sync(ctx, GPU_COMMANDS_COMPLETE, 0);
        assert(loaded >> 32 > next >> 32 && loaded >> 32 > b >> 32);
        snx_gl_delete_sync(ctx, next);
        snx_gl_delete_sync(ctx, loaded);
    }
    /* a fence for each texture a game uploads, alive until the texture is first drawn: 2000 at once (a game made 1267
       textures), each its own; past the table, 0 with GL_OUT_OF_MEMORY, said once with the count */
    {
        uint32_t n = SNX_GL_SYNCS > 2000 ? SNX_GL_SYNCS : 2000;
        uint64_t* kept = calloc(n, sizeof *kept);
        char full[64];
        assert(kept);
        snx_gl_delete_sync(ctx, b);
        for (uint32_t i = 0; i < 2000; i++) {
            kept[i] = snx_gl_fence_sync(ctx, GPU_COMMANDS_COMPLETE, 0);
            assert(kept[i] && !snx_gl_get_error(ctx) && (!i || kept[i] != kept[i - 1]));
        }
        assert(snx_gl_is_sync(kept[0]) && snx_gl_is_sync(kept[1999]));
        for (uint32_t i = 2000; i < SNX_GL_SYNCS; i++)
            assert((kept[i] = snx_gl_fence_sync(ctx, GPU_COMMANDS_COMPLETE, 0)));
        assert(!snx_gl_fence_sync(ctx, GPU_COMMANDS_COMPLETE, 0) && snx_gl_get_error(ctx) == 0x0505);
        assert(!snx_gl_fence_sync(ctx, GPU_COMMANDS_COMPLETE, 0) && snx_gl_get_error(ctx) == 0x0505);
        snprintf(full, sizeof full, "holds %d syncs, all alive", SNX_GL_SYNCS);
        assert(g_gl_sync_test.said == 1 && strstr(g_gl_sync_test.last_said, full));
        for (uint32_t i = 0; i < SNX_GL_SYNCS; i++) snx_gl_delete_sync(ctx, kept[i]);
        assert(!snx_gl_get_error(ctx));
        free(kept);
    }
    /* a platform with no work: every fence signaled */
    snx_gl_sync_host(NULL);
    a = snx_gl_fence_sync(ctx, GPU_COMMANDS_COMPLETE, 0);
    assert(snx_gl_client_wait_sync(ctx, a, 0, 0) == ALREADY_SIGNALED);
    puts("gl_sync: ok");
    return 0;
}
