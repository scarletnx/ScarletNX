/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* nn::prepo, native (prepo.hook): the play reports, with no prepo:u session. The behavior is that of a clean-room
   spec of the SDK's functions, never the SDK's code.

   The game builds a report in its own buffer through the SDK's own code (the event id, then a MessagePack map of its
   values), which needs no service and runs as it is. Only Save and RequestImmediateTransmission reach the service.
   Nothing here sends a report anywhere: Save checks the event id, logs it, and starts the buffer again as the SDK does
   after a send, and RequestImmediateTransmission has nothing to send.

   SDK 20 limits Save to 900 calls in any 90 minutes: a call past the limit returns success and leaves the report as
   it is. The SDK also signals its own event for that (GetOverSaveFrequencyLimitEvent), which stays with the SDK's
   code and is not signaled here. Any other nn::prepo function that needs the service opens prepo:u, and sm stops the
   game with the name of this layer */
#include "nx/nn/prepo.h"
#include "nx/hle/hook/hook.h"
#include "hle/hook/hooks.hook.h"
#include "nx/hle/hle.h"
#include "nx/runtime/clock.h"
#include "nx/runtime/platform/log.h"
#include "common/platform/thread.h"

#define PREPO_EVENT_ID_INVALID 0xa81u                      /* the play report module (129), description 5 */
#define PREPO_LIMIT_NS         (5400ull * 1000000000ull)    /* 90 minutes */
#define PREPO_EVENT_ID         0x20                         /* the event id at the report's start: 31 characters at most */

/* the host's side, which a save state never holds */
static struct {
    PMutex lock; /* the limit's ring */
} g_nnprepo;

void nnprepo_init(void) { pm_init(&g_nnprepo.lock); }

/* an event id: 1 to 31 of 0-9, a-z and _ */
static int event_id_ok(const CPU* c, u64 report) {
    const char* id = (const char*)hook_host(c, report);
    int n = 0;
    while (n < PREPO_EVENT_ID && id[n]) {
        char ch = id[n++];
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || ch == '_')) return 0;
    }
    return n > 0 && n < PREPO_EVENT_ID;
}

/* the report's buffer started again: an empty MessagePack map (de 00 00), when it has room for one */
static void clear(CPU* c, const HookLayout_nn_prepo_PlayReport* L, u64 report) {
    u64 buffer = hook_get_addr(c, L->buffer, report), capacity = hook_get_u64(c, L->capacity, report);
    if (!buffer || capacity < 3) {
        hook_put_u64(c, L->used, report, 0);
        return;
    }
    u8* p = hook_host(c, buffer);
    p[0] = 0xde, p[1] = 0, p[2] = 0;
    hook_put_u64(c, L->used, report, 3);
}

/* a save past SDK 20's limit: the 900th save before this one less than 90 minutes ago */
static int over_limit(void) {
    u64 now = guest_ns();
    pm_lock(&g_nnprepo.lock);
    HlePrepo* s = &g_hle.prepo;
    u64 slot = s->saved % PREPO_SAVES;
    int over = s->saved >= PREPO_SAVES && now - s->at[slot] < PREPO_LIMIT_NS;
    if (!over) {
        s->at[slot] = now;
        s->saved++;
    }
    pm_unlock(&g_nnprepo.lock);
    return over;
}

static u32 save(CPU* c, const HookLayout_nn_prepo_PlayReport* L, u64 report) {
    if (!event_id_ok(c, report)) return PREPO_EVENT_ID_INVALID;
    LOGV("prepo: the report %.31s (%llu bytes) is kept nowhere\n", (const char*)hook_host(c, report),
         (unsigned long long)hook_get_u64(c, L->used, report));
    clear(c, L, report);
    return 0;
}
u32 nnprepo_save(CPU* c, const HookLayout_nn_prepo_PlayReport* L, u64 report) {
    return over_limit() ? 0 : save(c, L, report);
}
u32 nnprepo_save_user(CPU* c, const HookLayout_nn_prepo_PlayReport* L, u64 report, u64 uid) {
    (void)uid;
    return nnprepo_save(c, L, report);
}
/* SDK 7.7.0: no limit */
u32 nnprepo_save_unlimited(CPU* c, const HookLayout_nn_prepo_PlayReport* L, u64 report) { return save(c, L, report); }
u32 nnprepo_save_user_unlimited(CPU* c, const HookLayout_nn_prepo_PlayReport* L, u64 report, u64 uid) {
    (void)uid;
    return save(c, L, report);
}
u32 nnprepo_request_immediate_transmission(CPU* c) {
    (void)c;
    return 0;
}
