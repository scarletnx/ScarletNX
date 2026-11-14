/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* gpu/renderer.c's side of the front end, with no device: the renderer reads a copy of each front end's draw state
   (DsState), which takes only the groups whose generation moved. A change that the front end makes after a call does
   not reach the copy that the call gave. gpu_draw counts the draws for the made-up counters, and gpu_dispatch flushes
   a merged draw before a dispatch with compute_dispatches */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nx/gpu/renderer.c"

/* ------------------------------------------------ what renderer.c reaches outside itself */
static int logged;
void logf_(int lvl, const char* fmt, ...) {
    (void)lvl, (void)fmt;
    logged++;
}
static int flushes;
void r3d_pending_flush(void) { flushes++; }

/* the stand-in renderer: what each call saw */
static DsState seen_ds;
static const DsState* seen_ptr;
static int ndraw, nclear, ntex, ndiscard, ndispatch;
static u32 seen_targets;
static void r_draw(const DsState* ds, const DrawCall* dc, DrawOwner* o) {
    (void)dc, (void)o;
    seen_ptr = ds, seen_ds = *ds, ndraw++;
}
static void r_clear(const DsState* ds, const DrawClear* c, const DrawOwner* o) {
    (void)c, (void)o;
    seen_ptr = ds, seen_ds = *ds, nclear++;
}
static void r_texture(const DsState* ds, const DrawTexture* t) {
    (void)t;
    seen_ptr = ds, seen_ds = *ds, ntex++;
}
static void r_discard(const DsState* ds, u32 targets) {
    seen_ptr = ds, seen_ds = *ds, seen_targets = targets, ndiscard++;
}
static int flushes_at_dispatch;
static void r_dispatch(const DispatchState* d) {
    (void)d;
    flushes_at_dispatch = flushes, ndispatch++;
}
static const Renderer stand_in = {.name = "stand-in",
                                  .draw = r_draw,
                                  .clear = r_clear,
                                  .draw_texture = r_texture,
                                  .discard = r_discard,
                                  .dispatch = r_dispatch};
const Renderer* g_renderer = &stand_in;

static int failed, checks;
#define CHECK(cond, ...)                                                                                               \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(cond)) {                                                                                                 \
            failed++;                                                                                                  \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                                                       \
            fprintf(stderr, __VA_ARGS__);                                                                              \
            fputc('\n', stderr);                                                                                       \
        }                                                                                                              \
    } while (0)

/* the first byte of a group */
#define B(s, group) (((u8*)&(s).group)[0])

static DsState a, b; /* two front ends' states */
static u64 gen_next = 1;
static void new_state(DsState* s, u32 owner, u8 fill) {
    memset(s, fill, sizeof *s);
    s->owner = owner;
    for (u32 g = 0; g < DS_GENS; g++) s->gen[g] = gen_next++;
}

static void test_copy(void) {
    DrawCall dc = {0};
    DrawOwner own = {0};
    new_state(&a, 1, 0x11);
    u64 draws = g_draws;
    gpu_draw(&a, &dc, &own);
    CHECK(ndraw == 1, "the draw reached the renderer %d times", ndraw);
    CHECK(g_draws == draws + 1, "gpu_draw counts the draw: %llu after %llu", (unsigned long long)g_draws,
          (unsigned long long)draws);
    CHECK(seen_ptr && seen_ptr != &a, "the renderer reads a copy, not the front end's state");
    CHECK(!memcmp(&seen_ds, &a, sizeof a), "a new state's copy is whole");

    /* the copy that the draw gave doesn't change with the front end's next methods */
    const DsState* copy = seen_ptr;
    B(a, blend) ^= 0xFF;
    a.gen[DS_BLEND] = gen_next++;
    CHECK(B(*copy, blend) == 0x11, "the front end's change reached the copy before the next call");

    /* the next call takes the group whose generation moved */
    gpu_clear(&a, &(DrawClear){0}, &own);
    CHECK(nclear == 1, "the clear reached the renderer");
    CHECK(B(seen_ds, blend) == (u8)(0x11 ^ 0xFF), "the copy did not take the blend group of a new generation");
    CHECK(seen_ds.gen[DS_BLEND] == a.gen[DS_BLEND], "the copy did not take the blend group's generation");

    /* a group whose bytes changed under the same generation keeps the bytes the copy has: the copy took only the
       groups whose generation moved */
    B(a, inl) = 0x5A;
    B(a, vp) = 0x77;
    a.gen[DS_VIEWPORTS] = gen_next++;
    gpu_draw_texture(&a, &(DrawTexture){0});
    CHECK(ntex == 1, "the texture draw reached the renderer");
    CHECK(B(seen_ds, inl) == 0x11, "the copy took a group whose generation did not move");
    CHECK(B(seen_ds, vp) == 0x77, "the copy did not take the viewports of a new generation");

    /* the owner always goes with the call */
    a.owner = 9;
    gpu_discard(&a, 5);
    CHECK(ndiscard == 1 && seen_targets == 5, "the discard reached the renderer with its targets");
    CHECK(seen_ds.owner == 9, "the copy did not take the owner");

    /* another front end's state has a copy of its own, and the first keeps its own */
    new_state(&b, 2, 0x22);
    gpu_draw(&b, &dc, &own);
    CHECK(seen_ptr != copy && !memcmp(&seen_ds, &b, sizeof b), "a second state shares the first one's copy");
    gpu_draw(&a, &dc, &own);
    CHECK(seen_ptr == copy && seen_ds.owner == 9 && B(seen_ds, vp) == 0x77, "the first state lost its copy");

    /* draw_state_check: a group changed under its generation is logged, and the copy takes it */
    g_settings.draw_state_check = 1;
    B(a, prog) ^= 0x01;
    logged = 0;
    gpu_draw(&a, &dc, &own);
    /* the programs and the inline group, which changed above under their generations */
    CHECK(logged == 2, "draw_state_check logged %d groups, not the two changed under their generations", logged);
    CHECK(!memcmp(&seen_ds, &a, sizeof a), "draw_state_check did not make the copy equal to the state");
    g_settings.draw_state_check = 0;
}

static void test_dispatch(void) {
    DispatchState d = {0};
    g_settings.compute_dispatches = 1;
    flushes = 0;
    gpu_dispatch(&d);
    CHECK(ndispatch == 1, "the dispatch reached the renderer");
    CHECK(flushes_at_dispatch == 1, "a merged draw was not flushed before the dispatch (%d)", flushes_at_dispatch);
    g_settings.compute_dispatches = 0;
    gpu_dispatch(&d);
    CHECK(flushes == 1, "a dispatch flushed a merged draw without compute_dispatches");
}

int main(void) {
    test_copy();
    test_dispatch();
    if (failed) {
        fprintf(stderr, "gpu_records: %d of %d checks failed\n", failed, checks);
        return 1;
    }
    printf("gpu_records: %d checks passed\n", checks);
    return 0;
}
