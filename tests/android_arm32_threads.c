/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A 32-bit guest's threads and the host's that run its code (src/android/bridge/arm32_thread.c, included whole for its
   statics): a thread's record holds where its thread-locals keep the signals it has pending, which end with it. A
   signal to a thread that has ended is ESRCH and writes nothing, and a host thread that the guest did not make (a
   JVM's) leaves neither its record nor its guest stack, so that the window does not fill with the stacks of a pool's
   threads that came and went */
#include "android/bridge/arm32_thread.c"
#include <assert.h>

static size_t records(void) {
    size_t n = 0;
    pthread_mutex_lock(&th.lock);
    for (const ThreadInfo* t = th.threads; t; t = t->next) ++n;
    pthread_mutex_unlock(&th.lock);
    return n;
}
static void* host_thread(void* p) {
    ThreadInfo* t = me(); /* a JVM's thread, the first time guest code asks it */
    assert(t && t->pending == &arm32_pending_signals);
    *(uint32_t*)p = t->tid;
    return NULL;
}
static void* host_cpu(void* p) {
    (void)p;
    assert(arm32_cpu());
    return NULL;
}
static void run(void* (*fn)(void*), void* arg) {
    pthread_t t;
    assert(!pthread_create(&t, NULL, fn, arg));
    assert(!pthread_join(t, NULL));
}
/* guest words {ret} as A32 code, as the bridge puts its own into the window */
static uint32_t guest_call(const char* name, const uint32_t* words, unsigned count) {
    return arm32_call(arm32_hle_symbol(name), words, count, NULL);
}

int main(void) {
    char error[256] = "";
    if (!arm32_init(error, sizeof error)) {
        fprintf(stderr, "android_arm32_threads: the 32-bit bridge does not start: %s\n", error);
        return 1;
    }
    (void)me();
    size_t before = records();
    uint32_t tid = 0;
    run(host_thread, &tid);
    assert(records() == before); /* its record went with it */
    assert(send_tid(tid, 10) == -L_ESRCH);

    /* a guest's own thread, ended and not joined: kept for its join, with no signal of it to run */
    static const uint32_t code[] = {0xe3a00000u /* mov r0, #0 */, 0xe12fff1eu /* bx lr */};
    uint32_t start = arm32_guest_code(code, 2), out = arm32_malloc(4);
    assert(guest_call("pthread_create", (const uint32_t[]){out, 0, start, 0}, 4) == 0);
    uint32_t self = rd32(out);
    ThreadInfo* t = NULL;
    for (bool done = false; !done; pthread_mutex_unlock(&th.lock)) {
        pthread_mutex_lock(&th.lock);
        t = by_self(self);
        assert(t);
        done = t->done;
    }
    assert(!t->pending);
    assert(guest_call("pthread_kill", (const uint32_t[]){self, 10}, 2) == L_ESRCH);
    assert(records() == before + 1);
    assert(guest_call("pthread_join", (const uint32_t[]){self, 0}, 2) == 0);
    assert(records() == before);

    /* 600 host threads with guest stacks of 8 MiB each, one at a time: 4.7 GiB, of a window of 4 */
    for (int i = 0; i < 600; ++i) run(host_cpu, NULL);
    puts("android_arm32_threads: ok");
    return 0;
}
