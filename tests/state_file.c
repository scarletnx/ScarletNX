/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A save state's file (src/nx/runtime/state_file.c): what is written reads back the same, section by section and in
   any order; the header and the thumbnail read without the sections; a section that isn't there, a byte changed in a
   section's data, or a state that wasn't written to its end, is refused; and a save that fails leaves the state that
   was at the path */
#define _POSIX_C_SOURCE 200809L /* pread, fileno, ftruncate */
#include "nx/runtime/state_file.h"
#include "nx/runtime/platform/plat.h"
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* the platform's, as plat_posix.c has them */
i64 plat_pread(FILE* f, void* buf, u64 size, u64 off) { return (i64)pread(fileno(f), buf, (size_t)size, (off_t)off); }
int plat_replace_file(const char* tmp, const char* dst) {
    if (!rename(tmp, dst)) return 0;
    remove(tmp);
    return -1;
}
/* the writer's workers: pthreads, four CPUs */
void pm_init(PMutex* m) { pthread_mutex_init(&m->l, NULL); }
void pm_destroy(PMutex* m) { pthread_mutex_destroy(&m->l); }
void pm_lock(PMutex* m) { pthread_mutex_lock(&m->l); }
void pm_unlock(PMutex* m) { pthread_mutex_unlock(&m->l); }
void pc_init(PCond* c) { pthread_cond_init(&c->c, NULL); }
void pc_signal(PCond* c) { pthread_cond_signal(&c->c); }
void pc_broadcast(PCond* c) { pthread_cond_broadcast(&c->c); }
int pc_wait(PCond* c, PMutex* m, i64 timeout_ns) {
    (void)timeout_ns; /* the writer waits without limit */
    pthread_cond_wait(&c->c, &m->l);
    return 0;
}
typedef struct {
    PThreadFn fn;
    void* arg;
} Start;
static void* start(void* p) {
    Start s = *(Start*)p;
    free(p);
    s.fn(s.arg);
    return NULL;
}
int plat_thread(PThreadFn fn, void* arg, size_t stack) {
    pthread_t t;
    Start* s = (Start*)malloc(sizeof *s);
    (void)stack;
    if (!s) return -1;
    s->fn = fn;
    s->arg = arg;
    if (pthread_create(&t, NULL, start, s)) {
        free(s);
        return -1;
    }
    pthread_detach(t);
    return 0;
}
int plat_cpus(int* ids, int max) {
    int i;
    for (i = 0; i < 4 && i < max; i++) ids[i] = i;
    return i;
}

static unsigned checks, failures;
#define CHECK(expr)                                                                                                    \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(expr)) {                                                                                                 \
            failures++;                                                                                                \
            fprintf(stderr, "state_file.c:%d: %s\n", __LINE__, #expr);                                                 \
        }                                                                                                              \
    } while (0)

int main(int argc, char** argv) {
    char path[512], why[256] = "";
    static u8 big[10 << 20], back[10 << 20]; /* two and a half of the writer's 4 MiB frames */
    u8 thumb[4 * 4 * 4], *t = NULL;
    SnxStateHeader h, got;
    StateWriter* w;
    StateReader* r;
    u64 schema = 0;
    u32 i, small = 0x12345678, v = 0;
    FILE* f;
    long size;
    snprintf(path, sizeof path, "%s/snx-test-state-%d.snxstate", argc > 1 ? argv[1] : ".", (int)getpid());
    for (i = 0; i < sizeof big; i++) big[i] = (u8)(i * 2654435761u >> 13) & (i & 0x100000 ? 0 : 0xFF);
    for (i = 0; i < sizeof thumb; i++) thumb[i] = (u8)i;
    memset(&h, 0, sizeof h);
    h.abi = 13;
    h.title_id = 0x0100EF901E552000ull;
    h.created = 1792700000;
    h.guest_ns = 123456789;
    h.thumb_w = h.thumb_h = 4;
    snprintf(h.runtime, sizeof h.runtime, "test build");
    snprintf(h.game, sizeof h.game, "A game");

    /* two sections, the big one put in pieces, compressed as frames side by side */
    w = sw_open(path, &h, thumb, why, sizeof why);
    CHECK(w != NULL);
    CHECK(!sw_begin(w, "SMALL", 7) && !sw_put(w, &small, sizeof small) && !sw_end(w));
    CHECK(!sw_begin(w, "MEM", 0xABCDEFull) && !sw_put(w, big, 1 << 20) &&
          !sw_put(w, big + (1 << 20), sizeof big - (1 << 20)));
    CHECK(!sw_end(w) && !sw_finish(w, why, sizeof why));

    /* the header and the thumbnail alone */
    CHECK(!snx_state_peek(path, &got, &t, why, sizeof why));
    CHECK(got.format == SNX_STATE_FORMAT && got.abi == 13 && got.title_id == h.title_id && got.guest_ns == h.guest_ns &&
          !strcmp(got.game, "A game") && !strcmp(got.runtime, "test build"));
    CHECK(t && !memcmp(t, thumb, sizeof thumb));
    free(t);

    /* the sections, the later one first, then the other in two reads */
    r = sr_open(path, &got, why, sizeof why);
    CHECK(r != NULL);
    CHECK(!sr_begin(r, "MEM", &schema) && schema == 0xABCDEFull && sr_left(r) == sizeof big);
    CHECK(!sr_get(r, back, 1000) && !sr_get(r, back + 1000, sizeof big - 1000) && !memcmp(back, big, sizeof big));
    CHECK(sr_get(r, back, 1) == -1); /* nothing left */
    CHECK(!sr_begin(r, "SMALL", &schema) && schema == 7 && !sr_get(r, &v, sizeof v) && v == small);
    CHECK(sr_begin(r, "GPU", &schema) == -1); /* not there */
    sr_close(r);

    /* a byte changed in the big section's data: its frame's checksum refuses it */
    f = fopen(path, "r+b");
    CHECK(f != NULL);
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, size / 2, SEEK_SET);
    v = (u32)fgetc(f);
    fseek(f, size / 2, SEEK_SET);
    fputc((int)(v ^ 0x40), f);
    fclose(f);
    r = sr_open(path, &got, why, sizeof why);
    CHECK(r != NULL && !sr_begin(r, "MEM", &schema) && sr_get(r, back, sizeof big) == -1);
    sr_close(r);

    /* a save that fails half way leaves the state at the path as it was: its .part goes */
    w = sw_open(path, &h, NULL, why, sizeof why);
    CHECK(w != NULL && !sw_begin(w, "SMALL", 1));
    sw_abort(w);
    CHECK(!snx_state_peek(path, &got, NULL, why, sizeof why) && got.thumb_w == 4);
    snprintf(why, sizeof why, "%s.part", path);
    CHECK(access(why, F_OK) != 0);

    /* a state cut short: its table is gone */
    f = fopen(path, "r+b");
    CHECK(f != NULL && !ftruncate(fileno(f), size - 8));
    fclose(f);
    CHECK(sr_open(path, &got, why, sizeof why) == NULL && strstr(why, "end") != NULL);

    /* not a state at all */
    f = fopen(path, "wb");
    fputs("hello", f);
    fclose(f);
    CHECK(snx_state_peek(path, &got, NULL, why, sizeof why) == -1);
    remove(path);

    printf("state_file: %u checks, %u failed\n", checks, failures);
    return failures != 0;
}
