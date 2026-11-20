/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The GL layer's sync objects (gl_state.h's layer), shared by the Android bridge and the console.

   A fence is the platform's serial of the work recorded when it was made: signaled once the platform says it
   finished that serial. A platform that records no GPU work (the console's layer before it draws) gives none, and
   every fence is signaled at once. The records are one block a save state holds; a loaded fence is signaled, since
   the work before a save state is done. The caller holds its platform's GL lock */
#ifndef SNX_GL_SYNC_H
#define SNX_GL_SYNC_H
#include <stddef.h>
#include <stdint.h>

/* fences alive at once: a game keeps one for each texture it uploads until the texture is first drawn (1267 at once),
   so as many as the layer holds textures and buffers */
#define SNX_GL_SYNCS 12288

/* A sync's handle is its id in the high word and its record's number from 1 in the low: an id is never given twice,
   so a handle the game kept past a delete names no sync, though the record is used again (a GLsync is a pointer, which
   a driver doesn't give out again at once, and a game may test a deleted one with glIsSync) */
typedef struct {
    uint32_t used, id;
    uint64_t serial; /* the platform's work it waits for, 0 for none */
} SnxGlSync;

/* What a platform tells of its work; each may be NULL. Not part of a save state: given again after a load */
typedef struct {
    uint64_t (*recorded)(void);  /* the serial of the work recorded so far */
    uint64_t (*completed)(void); /* the serial of the work the GPU finished */
    /* wait until the GPU finished serial, the context's recorded work submitted first */
    void (*wait)(uint32_t ctx, uint64_t serial);
    void (*say)(const char* line); /* a line for the log: the first glFenceSync that finds every record alive */
} SnxGlSyncHost;
void snx_gl_sync_host(const SnxGlSyncHost* host);

/* glFenceSync: the fence's handle, 0 with the error recorded */
uint64_t snx_gl_fence_sync(uint32_t ctx, uint32_t condition, uint32_t flags);
void snx_gl_delete_sync(uint32_t ctx, uint64_t sync);
int snx_gl_is_sync(uint64_t sync);
/* glClientWaitSync: GL_ALREADY_SIGNALED, GL_TIMEOUT_EXPIRED, GL_CONDITION_SATISFIED or GL_WAIT_FAILED (the error
   recorded). A timeout other than 0 waits until the fence is signaled */
uint32_t snx_gl_client_wait_sync(uint32_t ctx, uint64_t sync, uint32_t flags, uint64_t timeout);
/* glWaitSync: the GPU's own wait, which one queue in order needs not */
void snx_gl_wait_sync(uint32_t ctx, uint64_t sync, uint32_t flags, uint64_t timeout);
/* glGetSynciv: 1 with the value in *v, 0 with the error recorded */
int snx_gl_get_sync(uint32_t ctx, uint64_t sync, uint32_t pname, int32_t* v);

/* the save state's part (after the framebuffers'): its bytes, a copy, the check and its load */
size_t snx_gl_sync_state_bytes(void);
void snx_gl_sync_state_save(void* out);
int snx_gl_sync_state_check(const void* in, size_t size, char* why, size_t whysize);
void snx_gl_sync_state_load(const void* in);

#endif
