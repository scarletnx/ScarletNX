/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* nn::prepo, native (prepo.hook): the play reports, with no prepo:u session. The behavior is that of a clean-room
   spec of the SDK's functions, never the SDK's code.

   The game builds a report in its own buffer through the SDK's own code (the event id, then a MessagePack map of its
   values), which needs no service and runs as it is. Save, RequestImmediateTransmission and GetTransmissionStatus
   reach the service. Nothing here sends a report anywhere: Save checks the event id, logs it, and starts the buffer
   again as the SDK does after a send, RequestImmediateTransmission has nothing to send, and the status is that of an
   empty queue.

   From SDK 16 a game also sends a report at once (PlayReport::Send), which hands the report's buffer to the service and gives
   the game an AsyncContext to wait on. With no service the context is the SDK's own finished one, no object and a
   Result, as its AsyncContext(Result) makes: the SDK's GetCompletionEvent gives such a context a signaled event,
   GetResult its Result and Cancel nothing. Send has the limit of its own, 900 in any 90 minutes, past which the
   context holds the limit's Result and the report is left; an event id Save refuses stops the SDK's Send, so that
   case runs the SDK's own code.

   A 32-bit game's SDK (0.12.16, the one read) has the same Save over a report of 4-byte fields (PlayReport32).

   From SDK 10 Save has a limit, 900 calls in any 90 minutes as SDK 20's is: a call past the limit returns success and
   leaves the report as it is. The SDK also signals its own event for that (GetOverSaveFrequencyLimitEvent), which stays with the SDK's
   code and is not signaled here.

   From SDK 10 AddSessionId adds a report's session id: an Any64BitId the SDK keeps, drawn at random whenever the
   system's session id it asks the service for differs from the last one it saw, which starts as 0. The service gave 0
   for that id (prepo:u answered so before it left, as Eden does), so the SDK's id stayed 0, and here too: 8 zero bytes,
   added by the SDK's own PlayReport::Add.

   Any other nn::prepo function that needs the service takes the session object the SDK's start did not make (its
   getter gives none in SDK 3.5 to 20.5), and the game crashes in it. No game of the dumps read calls one: they are the
   storage, the statistics, the upload history, the user agreement check, the operation mode and the system's reports */
#include "nx/nn/prepo.h"
#include "nx/hle/hook/hook.h"
#include "hle/hook/hooks.hook.h"
#include "nx/hle/hle.h"
#include "nx/hle/kernel/kernel.h"
#include "nx/runtime/clock.h"
#include "nx/runtime/platform/log.h"
#include "common/platform/thread.h"

#define PREPO_SEND_LIMIT       0xc81u /* the play report module (129), description 6 */
#define PREPO_SYM_SEND         "_ZN2nn5prepo10PlayReport4SendERKNS_7account3UidE"
#define PREPO_SYM_ADD_ID       "_ZN2nn5prepo10PlayReport3AddEPKcRKNS0_10Any64BitIdE"
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

/* a report's buffer started again: an empty MessagePack map (de 00 00), when it has room for one. What the report
   has used of it then */
static u32 restart(CPU* c, u64 buffer, u64 capacity) {
    if (!buffer || capacity < 3) return 0;
    u8* p = hook_host(c, buffer);
    p[0] = 0xde, p[1] = 0, p[2] = 0;
    return 3;
}
static void clear(CPU* c, const HookLayout_nn_prepo_PlayReport* L, u64 report) {
    hook_put_u64(c, L->used, report,
                 restart(c, hook_get_addr(c, L->buffer, report), hook_get_u64(c, L->capacity, report)));
}

/* a save or a send past the limit: the 900th before this one less than 90 minutes ago, in the ring of its kind */
static int over_ring(u64* count, u64* at) {
    u64 now = guest_ns();
    pm_lock(&g_nnprepo.lock);
    u64 slot = *count % PREPO_SAVES;
    int over = *count >= PREPO_SAVES && now - at[slot] < PREPO_LIMIT_NS;
    if (!over) {
        at[slot] = now;
        ++*count;
    }
    pm_unlock(&g_nnprepo.lock);
    return over;
}
static int over_limit(void) { return over_ring(&g_hle.prepo.saved, g_hle.prepo.at); }

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
/* an SDK before the limit */
u32 nnprepo_save_unlimited(CPU* c, const HookLayout_nn_prepo_PlayReport* L, u64 report) { return save(c, L, report); }
u32 nnprepo_save_user_unlimited(CPU* c, const HookLayout_nn_prepo_PlayReport* L, u64 report, u64 uid) {
    (void)uid;
    return save(c, L, report);
}
/* a 32-bit SDK's report: the same fields, 4 bytes each. Its Save has no limit */
u32 nnprepo_save32(CPU* c, const HookLayout_nn_prepo_PlayReport32* L, u64 report) {
    if (!event_id_ok(c, report)) return PREPO_EVENT_ID_INVALID;
    LOGV("prepo: the report %.31s (%u bytes) is kept nowhere\n", (const char*)hook_host(c, report),
         (unsigned)hook_get_u32(c, L->used, report));
    hook_put_u32(c, L->used, report,
                 restart(c, hook_get_u32(c, L->buffer, report), hook_get_u32(c, L->capacity, report)));
    return 0;
}
u32 nnprepo_save_user32(CPU* c, const HookLayout_nn_prepo_PlayReport32* L, u64 report, u64 uid) {
    (void)uid;
    return nnprepo_save32(c, L, report);
}
u32 nnprepo_request_immediate_transmission(CPU* c) {
    (void)c;
    return 0;
}
/* nothing is queued: the console's status with an empty queue, 0, as Eden answers the service's command */
u32 nnprepo_get_transmission_status(CPU* c, u64 status) {
    snx_st_le32(hook_host(c, status), 0);
    return 0;
}
/* nn::prepo::detail::ipc's Initialize and Finalize, where nn::prepo::Initialize and the library's first use open
   prepo:u and its end closes it: there is no session to open, and every function that would use it is answered here */
void nnprepo_ipc_initialize(CPU* c) { (void)c; }
void nnprepo_ipc_finalize(CPU* c) { (void)c; }

/* a finished context at out: no service object, and r */
static void context(CPU* c, const HookLayout_nn_prepo_AsyncContext* A, u64 out, u32 r) {
    hook_put_addr(c, A->object, out, 0);
    hook_put_u32(c, A->result, out, r);
}
/* PlayReport::Send, whose AsyncContext the caller passes in x8: the limit first, as the SDK orders them. A report
   sent leaves the object with no buffer, as the SDK's does once the service has taken it */
void nnprepo_send(CPU* c, const HookLayout_nn_prepo_PlayReport* L, const HookLayout_nn_prepo_AsyncContext* A,
                  u64 report, u64 uid) {
    u64 out = c->x[8];
    (void)uid;
    if (over_ring(&g_hle.prepo.sent, g_hle.prepo.sent_at)) {
        context(c, A, out, PREPO_SEND_LIMIT);
        return;
    }
    if (!event_id_ok(c, report)) {
        u64 at = hook_site(PREPO_SYM_SEND);
        if (!at) fatal("nn::prepo: %s has no code of its own to run", PREPO_SYM_SEND);
        hook_run_original(c, at);
        return;
    }
    LOGV("prepo: the report %.31s (%llu bytes), sent at once, is kept nowhere\n", (const char*)hook_host(c, report),
         (unsigned long long)hook_get_u64(c, L->used, report));
    hook_put_addr(c, L->buffer, report, 0);
    context(c, A, out, 0);
}

/* AddSessionId: the session's id, 8 zero bytes, put below the caller's stack, where no frame is, and added under key by
   the SDK's own PlayReport::Add(key, Any64BitId), whose Result it gives */
u32 nnprepo_add_session_id(CPU* c, u64 report, u64 key) {
    u64 add = kernel_find_symbol_exact(PREPO_SYM_ADD_ID), sp = c->x[31], id = (sp - 8) & ~15ull;
    if (!add) fatal("nn::prepo: the SDK exports no %s, which AddSessionId adds the id with", PREPO_SYM_ADD_ID);
    snx_st_le64(hook_host(c, id), 0);
    c->x[31] = id;
    hook_guest_call(c, add, report, key, id, 0);
    c->x[31] = sp;
    return (u32)c->x[0];
}
