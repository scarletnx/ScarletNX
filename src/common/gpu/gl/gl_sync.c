/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The GL layer's sync objects (gl_sync.h) */
#include "common/gpu/gl/gl_sync.h"
#include "common/gpu/gl/gl_state.h"
#include <stdio.h>
#include <string.h>

enum {
    GL_OUT_OF_MEMORY_ = 0x0505,
    GL_SYNC_FLUSH_COMMANDS_BIT_ = 0x1,
    GL_OBJECT_TYPE_ = 0x9112,
    GL_SYNC_CONDITION_ = 0x9113,
    GL_SYNC_STATUS_ = 0x9114,
    GL_SYNC_FLAGS_ = 0x9115,
    GL_SYNC_FENCE_ = 0x9116,
    GL_SYNC_GPU_COMMANDS_COMPLETE_ = 0x9117,
    GL_UNSIGNALED_ = 0x9118,
    GL_SIGNALED_ = 0x9119,
    GL_ALREADY_SIGNALED_ = 0x911a,
    GL_TIMEOUT_EXPIRED_ = 0x911b,
    GL_CONDITION_SATISFIED_ = 0x911c,
    GL_WAIT_FAILED_ = 0x911d,
};
#define GL_TIMEOUT_IGNORED_ 0xffffffffffffffffull

static struct {
    SnxGlSync s[SNX_GL_SYNCS]; /* what a save state holds */
    SnxGlSyncHost host;
    uint32_t last_id; /* the id the last sync made took (a load takes the highest of its records) */
    int said;         /* the table full was told */
} g_gl_sync;

#define S g_gl_sync.s
#define H g_gl_sync.host

void snx_gl_sync_host(const SnxGlSyncHost* host) { H = host ? *host : (SnxGlSyncHost){0}; }

static SnxGlSync* sync_of(uint64_t sync) {
    uint32_t n = (uint32_t)sync;
    return n && n <= SNX_GL_SYNCS && S[n - 1].used && S[n - 1].id == (uint32_t)(sync >> 32) ? &S[n - 1] : NULL;
}
static int signaled(const SnxGlSync* s) { return !s->serial || (H.completed && s->serial <= H.completed()); }

uint64_t snx_gl_fence_sync(uint32_t ctx, uint32_t condition, uint32_t flags) {
    if (condition != GL_SYNC_GPU_COMMANDS_COMPLETE_ || flags) {
        snx_gl_error(ctx, condition != GL_SYNC_GPU_COMMANDS_COMPLETE_ ? SNX_GL_INVALID_ENUM : SNX_GL_INVALID_VALUE);
        return 0;
    }
    for (uint32_t i = 0; i < SNX_GL_SYNCS; i++)
        if (!S[i].used) {
            if (!++g_gl_sync.last_id) g_gl_sync.last_id = 1; /* past 2^32 syncs: 0 names none */
            S[i] = (SnxGlSync){.used = 1, .id = g_gl_sync.last_id, .serial = H.recorded ? H.recorded() : 0};
            return (uint64_t)g_gl_sync.last_id << 32 | (i + 1);
        }
    if (!g_gl_sync.said && H.say) {
        char line[160];
        snprintf(line, sizeof line,
                 "glFenceSync: the layer holds %d syncs, all alive (none deleted): GL_OUT_OF_MEMORY, the game gets 0",
                 SNX_GL_SYNCS);
        H.say(line);
    }
    g_gl_sync.said = 1;
    snx_gl_error(ctx, GL_OUT_OF_MEMORY_);
    return 0;
}
void snx_gl_delete_sync(uint32_t ctx, uint64_t sync) {
    SnxGlSync* s = sync_of(sync);
    if (s)
        *s = (SnxGlSync){0};
    else if (sync)
        snx_gl_error(ctx, SNX_GL_INVALID_VALUE);
}
int snx_gl_is_sync(uint64_t sync) { return sync_of(sync) != NULL; }
uint32_t snx_gl_client_wait_sync(uint32_t ctx, uint64_t sync, uint32_t flags, uint64_t timeout) {
    const SnxGlSync* s = sync_of(sync);
    if (!s || (flags & ~(uint32_t)GL_SYNC_FLUSH_COMMANDS_BIT_)) {
        snx_gl_error(ctx, SNX_GL_INVALID_VALUE);
        return GL_WAIT_FAILED_;
    }
    if (signaled(s)) return GL_ALREADY_SIGNALED_;
    if (!timeout) return GL_TIMEOUT_EXPIRED_;
    if (H.wait) H.wait(ctx, s->serial);
    return GL_CONDITION_SATISFIED_;
}
void snx_gl_wait_sync(uint32_t ctx, uint64_t sync, uint32_t flags, uint64_t timeout) {
    if (!sync_of(sync) || flags || timeout != GL_TIMEOUT_IGNORED_) snx_gl_error(ctx, SNX_GL_INVALID_VALUE);
}
int snx_gl_get_sync(uint32_t ctx, uint64_t sync, uint32_t pname, int32_t* v) {
    const SnxGlSync* s = sync_of(sync);
    if (!s) {
        snx_gl_error(ctx, SNX_GL_INVALID_VALUE);
        return 0;
    }
    switch (pname) {
        case GL_OBJECT_TYPE_: *v = GL_SYNC_FENCE_; return 1;
        case GL_SYNC_CONDITION_: *v = GL_SYNC_GPU_COMMANDS_COMPLETE_; return 1;
        case GL_SYNC_STATUS_: *v = signaled(s) ? GL_SIGNALED_ : GL_UNSIGNALED_; return 1;
        case GL_SYNC_FLAGS_: *v = 0; return 1;
        default: snx_gl_error(ctx, SNX_GL_INVALID_ENUM); return 0;
    }
}

size_t snx_gl_sync_state_bytes(void) { return 2 * sizeof(uint32_t) + sizeof S; }
void snx_gl_sync_state_save(void* out) {
    const uint32_t h[2] = {(uint32_t)sizeof S, 0};
    memcpy(out, h, sizeof h);
    memcpy((unsigned char*)out + sizeof h, S, sizeof S);
}
int snx_gl_sync_state_check(const void* in, size_t size, char* why, size_t whysize) {
    uint32_t h[2];
    if (size < sizeof h) {
        snprintf(why, whysize, "the GL layer's syncs are %zu bytes, less than their %zu-byte header", size, sizeof h);
        return -1;
    }
    memcpy(h, in, sizeof h);
    if (h[0] != sizeof S || size != sizeof h + sizeof S) {
        snprintf(why, whysize, "the GL layer's syncs are %u bytes of records (%zu in all); this build reads %zu",
                 (unsigned)h[0], size, sizeof S);
        return -1;
    }
    return 0;
}
void snx_gl_sync_state_load(const void* in) {
    memcpy(S, (const unsigned char*)in + 2 * sizeof(uint32_t), sizeof S);
    g_gl_sync.last_id = 0;
    for (uint32_t i = 0; i < SNX_GL_SYNCS; i++) {
        S[i].serial = 0; /* the work before the save state is done */
        if (S[i].used && S[i].id > g_gl_sync.last_id) g_gl_sync.last_id = S[i].id;
    }
}
