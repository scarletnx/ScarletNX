/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* gpu/renderer.c's side of the front end, with no device: the renderer reads a copy of each front end's draw state
   (DsState), which takes only the groups whose generation moved. A change that the front end makes after a call does
   not reach the copy that the call gave. gpu_draw counts the draws for the made-up counters, and gpu_dispatch flushes
   a merged draw before a dispatch with compute_dispatches.

   Each call reaches the renderer as a record of gpu_queue.c, in its order: the draw's inline indices are a copy, the
   renderer's side keeps its own DrawOwner with the front end's predicate and capture starts, and the calls without a
   result keep their arguments. The render enable is decided on the renderer's side, in the record's place. A front
   end that names the inline bytes it changed (gpu_ds_inline_changed) sends only those, while the ranges chain from the
   generation the renderer's side has, are at most 16 and less than half of the group.

   With the argument split, gpu_split's renderer thread runs the records: a draw that the front end queued reads the
   state as it was at the draw, while the front end has gone on and changed it */
#include <pthread.h>
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
void fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    exit(1);
}
/* gpu.c's side of the queue: this test is the front end's thread, and writes nothing */
int gpu_on_thread(void) { return 1; }
u64 gpu_thread_busy_ns(void) { return 0; }
int gmem_write_now(u64 gva, const void* src, u64 size) { return (void)gva, (void)src, (int)size; }
int gmem_write_cb_now(u64 gva, const void* src, u64 size) { return (void)gva, (void)src, (int)size; }
void gpu_release_write_now(u64 gva, const void* data, u32 len) { (void)gva, (void)data, (void)len; }
void gpu_release_ticks_now(u64 gva) { (void)gva; }
void gpu_release_stamp_ticked(u64 gva, const void* data, int top) { (void)gva, (void)data, (void)top; }
void gpu_i2m_block_now(const GpuI2m* t) { (void)t; }
void gmem_read_quiet(u64 gva, void* dst, u64 size) { (void)gva, memset(dst, 0, (size_t)size); }
/* maxwell/engines.c's render enable, as the renderer's side asks it: one owner decided, the others not */
static u32 re_owner = ~0u, re_pred;
static int re_off, re_resets;
int gpu_render_state(u32 owner, u32* pred) {
    if (owner != re_owner) return -1;
    *pred = re_pred;
    return re_off;
}
void gpu_render_states_reset(void) { re_resets++; }

/* the renderer's calls in their order, one letter each, and their arguments */
static char order[64];
static u32 norder;
static u32 args[64][2];
static void called(char c, u32 a, u32 b) {
    if (norder < 63) args[norder][0] = a, args[norder][1] = b, order[norder++] = c;
}

/* the stand-in renderer: what each call saw */
static int nxfb, nseen_mem;
static DrawOwner* xfb_own;
static u64 xfb_control_seen;
static u32 tex_pred;
static DsState seen_ds;
static const DsState* seen_ptr;
static int ndraw, nclear, ntex, ndiscard, ndispatch;
static u32 seen_targets;
static DrawOwner* seen_own;
static DrawOwner seen_own_v;
static u32 seen_index[8], seen_nindex;
static const u32* seen_index_ptr;
static void r_draw(const DsState* ds, const DrawCall* dc, DrawOwner* o) {
    seen_ptr = ds, seen_ds = *ds, ndraw++;
    seen_own = o, seen_own_v = *o;
    seen_index_ptr = dc->inline_indices;
    seen_nindex = dc->indexed == 3 && dc->inline_indices ? dc->first + dc->count : 0;
    for (u32 i = 0; i < seen_nindex && i < 8; i++) seen_index[i] = dc->inline_indices[i];
    o->xfb_reload = 0; /* the renderer starts the capture over, in its own copy */
    called('d', 0, 0);
}
static void r_clear(const DsState* ds, const DrawClear* c, const DrawOwner* o) {
    (void)c;
    seen_ptr = ds, seen_ds = *ds, nclear++;
    seen_own_v = *o;
    called('c', 0, 0);
}
static void r_texture(const DsState* ds, const DrawTexture* t) {
    tex_pred = t->owner.pred;
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
static int r_xfb(u32 owner, u64 control, u32 what, u32 stride, DrawOwner* o) {
    (void)owner, (void)what, (void)stride;
    xfb_own = o, xfb_control_seen = control, nxfb++;
    return 1;
}
static void r_mem_seen(void) { nseen_mem++; }
static void r_barrier(int kind) { called('b', (u32)kind, 0); }
static void r_incr(u32 id) { called('i', id, 0); }
static void r_end(void) { called('e', 0, 0); }
static void r_ready(void) { called('r', 0, 0); }
static void r_run(int on) { called('u', (u32)on, 0); }
static void r_reset(u32 owner) { called('s', owner, 0); }
static void r_enable(u32 owner, int on) { called('n', owner, (u32)on); }
static void r_drop(u32 pred) { called('p', pred, 0); }
static const Renderer stand_in = {.name = "stand-in",
                                  .draw = r_draw,
                                  .clear = r_clear,
                                  .draw_texture = r_texture,
                                  .discard = r_discard,
                                  .dispatch = r_dispatch,
                                  .method_barrier = r_barrier,
                                  .stream_incr = r_incr,
                                  .stream_end = r_end,
                                  .ready = r_ready,
                                  .stream_run = r_run,
                                  .samples_reset = r_reset,
                                  .samples_enable = r_enable,
                                  .render_predicate_drop = r_drop,
                                  .xfb_control = r_xfb,
                                  .mem_seen = r_mem_seen};
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
    s->pad = 0; /* no group holds it: a record sends the groups, the owner and the generations */
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
    gpu_draw(&a, &dc, &own);
    CHECK(!memcmp(&seen_ds, &a, sizeof a), "draw_state_check did not send the state whole after a difference");
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

/* the records of a draw: the indices and the owner */
static void test_records(void) {
    DrawOwner own = {0};
    u32 idx[6] = {10, 11, 12, 13, 14, 15};
    DrawCall dc = {.indexed = 3, .inline_indices = idx, .first = 1, .count = 4};
    new_state(&a, 3, 0x33);
    own.pred = 7;
    own.xfb_reload = 5;
    gpu_draw(&a, &dc, &own);
    CHECK(seen_nindex == 5 && !memcmp(seen_index, idx, 5 * 4), "the draw's inline indices did not arrive");
    CHECK(seen_index_ptr != idx, "the renderer reads the front end's indices, not a copy");
    CHECK(seen_own != &own, "the renderer writes the front end's DrawOwner, not its own");
    CHECK(seen_own_v.pred == 7 && seen_own_v.xfb_reload == 5,
          "the owner's predicate %u or capture starts %u did not arrive", seen_own_v.pred, seen_own_v.xfb_reload);
    CHECK(own.xfb_reload == 0, "the front end keeps capture starts that the renderer's side took");
    /* the renderer's own owner: the predicate follows the front end's, and a clear leaves the capture starts to the
       next draw */
    own.pred = 0;
    own.xfb_reload = 2;
    gpu_clear(&a, &(DrawClear){0}, &own);
    CHECK(seen_own_v.pred == 0, "the clear's owner kept an old predicate");
    CHECK(own.xfb_reload == 2, "a clear took the capture starts, which go with the next draw");
    dc.indexed = 0;
    gpu_draw(&a, &dc, &own);
    CHECK(seen_own_v.xfb_reload == 2, "the capture starts did not reach the draw after a clear (%u)",
          seen_own_v.xfb_reload);
    /* a state load: each state is sent whole and each owner starts over */
    seen_own->xfb_reload = 9;
    gpu_draw_states_reset();
    B(a, blend) = 0x44; /* under its generation: only a whole state brings it */
    own.xfb_reload = 0;
    gpu_draw(&a, &dc, &own);
    CHECK(B(seen_ds, blend) == 0x44, "after a state load, the state was not sent whole");
    CHECK(seen_own_v.xfb_reload == 0, "after a state load, the renderer's owner did not start over");
}

/* the inline group's changed ranges in place of the group. The test also changes a byte it doesn't name: the copy
   gets it only when the record carries the whole group */
#define INL(s, off) (((u8*)&(s).inl)[off])
static void inline_draw(DsState* s) {
    DrawOwner own = {0};
    gpu_draw(s, &(DrawCall){0}, &own);
}
static u64 inline_change(DsState* s, u32 off, u32 len, u8 v) {
    u64 before = s->gen[DS_INLINE];
    memset((u8*)&s->inl + off, v, len);
    s->gen[DS_INLINE] = gen_next++;
    return before;
}
static void test_inline_patches(void) {
    new_state(&b, 0x60, 0x66);
    inline_draw(&b); /* sent whole */
    /* two ranges, chained, and a byte not named */
    u64 g0 = inline_change(&b, 0x10, 8, 0x01);
    gpu_ds_inline_changed(&b, g0, 0x10, 8);
    u64 g1 = inline_change(&b, 0x2001, 3, 0x02);
    gpu_ds_inline_changed(&b, g1, 0x2001, 3);
    INL(b, 0x400) = 0x03;
    inline_draw(&b);
    CHECK(INL(seen_ds, 0x10) == 0x01 && INL(seen_ds, 0x17) == 0x01 && INL(seen_ds, 0x2001) == 0x02 &&
              INL(seen_ds, 0x2003) == 0x02,
          "the named ranges did not reach the renderer's copy");
    CHECK(INL(seen_ds, 0x400) == 0x66, "the record carried the whole inline group, not its changed ranges");
    CHECK(seen_ds.gen[DS_INLINE] == b.gen[DS_INLINE], "the copy did not take the inline group's generation");
    /* a block's own generation goes with the ranges: the renderer keeps a block's staged copy by it, and one left
       behind would draw the block's old bytes */
    u64 g3 = inline_change(&b, 0x10, 8, 0x07);
    b.inl_gen[0][0] = gen_next++;
    b.inl_gen[1][4] = gen_next++;
    b.root_gen = gen_next++;
    gpu_ds_inline_changed(&b, g3, 0x10, 8);
    inline_draw(&b);
    CHECK(INL(seen_ds, 0x10) == 0x07 && INL(seen_ds, 0x400) == 0x66, "the range did not go as a range");
    CHECK(!memcmp(seen_ds.inl_gen, b.inl_gen, sizeof b.inl_gen) && seen_ds.root_gen == b.root_gen,
          "a record of ranges left the inline blocks' generations behind");
    /* a range that follows no generation the renderer's side has: the whole group */
    INL(b, 0x400) = 0x66;
    inline_change(&b, 0x20, 4, 0x04);
    u64 g2 = inline_change(&b, 0x30, 4, 0x05); /* the change from g2's own generation, not the one sent */
    gpu_ds_inline_changed(&b, g2, 0x30, 4);
    INL(b, 0x400) = 0x06;
    inline_draw(&b);
    CHECK(INL(seen_ds, 0x400) == 0x06 && INL(seen_ds, 0x20) == 0x04, "a break in the chain did not send the group");
    /* 17 ranges apart: the whole group */
    u64 from = b.gen[DS_INLINE];
    for (u32 i = 0; i < 17; i++) {
        inline_change(&b, 0x800 + 0x40 * i, 4, (u8)(0x10 + i));
        gpu_ds_inline_changed(&b, from, 0x800 + 0x40 * i, 4);
        from = b.gen[DS_INLINE];
    }
    INL(b, 0x400) = 0x07;
    inline_draw(&b);
    CHECK(INL(seen_ds, 0x400) == 0x07, "17 ranges apart did not send the group");
    /* 17 ranges that touch: one patch */
    from = b.gen[DS_INLINE];
    for (u32 i = 0; i < 17; i++) {
        inline_change(&b, 0x1000 + 4 * i, 4, (u8)(0x30 + i));
        gpu_ds_inline_changed(&b, from, 0x1000 + 4 * i, 4);
        from = b.gen[DS_INLINE];
    }
    INL(b, 0x400) = 0x08;
    inline_draw(&b);
    CHECK(INL(seen_ds, 0x400) == 0x07 && INL(seen_ds, 0x1040) == 0x40 && INL(seen_ds, 0x1000) == 0x30,
          "ranges that touch did not go as one patch");
    /* half of the group or more: the whole group */
    from = b.gen[DS_INLINE];
    u32 half = (u32)sizeof b.inl / 2;
    inline_change(&b, 0, half, 0x09);
    gpu_ds_inline_changed(&b, from, 0, half);
    inline_draw(&b);
    CHECK(INL(seen_ds, 0x400) == 0x09 && !memcmp(&seen_ds.inl, &b.inl, sizeof b.inl),
          "a change of half the group did not send the group");
    /* a byte changed with no range named: the whole group */
    inline_change(&b, 0x3000, 1, 0x0A);
    inline_draw(&b);
    CHECK(!memcmp(&seen_ds.inl, &b.inl, sizeof b.inl), "a change with no range named did not send the group");
}

/* the calls without a result, in their order with the draws */
/* the render enable, decided on the renderer's side: the draws and clears of an owner it turns off are skipped, and
   the others take its predicate */
static void test_render_enable(void) {
    DrawOwner own = {0};
    DrawCall dc = {0};
    new_state(&b, 0x50, 0x55);
    re_owner = 0x50;
    re_off = 1;
    int draws = ndraw, clears = nclear;
    gpu_draw(&b, &dc, &own);
    gpu_clear(&b, &(DrawClear){0}, &own);
    CHECK(ndraw == draws && nclear == clears, "an owner's draws were not skipped while its render enable is off");
    gpu_draw(&a, &dc, &own);
    CHECK(ndraw == draws + 1, "another owner's draw was skipped");
    re_off = 0;
    re_pred = 6;
    own.pred = 0;
    gpu_draw(&b, &dc, &own);
    CHECK(ndraw == draws + 2 && seen_own_v.pred == 6, "a draw did not take its render enable's predicate (%u)",
          seen_own_v.pred);
    /* an owner whose draws are off: its textures and discards are skipped too, and its capture control runs */
    re_off = 1;
    int texs = ntex, discards = ndiscard;
    gpu_draw_texture(&b, &(DrawTexture){0});
    gpu_discard(&b, 1);
    CHECK(ntex == texs && ndiscard == discards, "a texture draw or a discard ran while its owner's draws are off");
    int xfbs = nxfb;
    gpu_xfb_control(&b, &own, 0x1234, 1, 16);
    CHECK(nxfb == xfbs + 1 && xfb_control_seen == 0x1234, "a capture control did not run while the draws are off");
    CHECK(xfb_own && xfb_own != &own, "a capture control ran with the front end's DrawOwner, not the renderer's");
    /* on, with a predicate: a texture draw takes it, and a discard waits for none, so it is skipped */
    re_off = 0;
    re_pred = 6;
    gpu_draw_texture(&b, &(DrawTexture){0});
    CHECK(ntex == texs + 1 && tex_pred == 6, "a texture draw did not take its render enable's predicate (%u)", tex_pred);
    gpu_discard(&b, 1);
    CHECK(ndiscard == discards, "a discard ran under a predicate");
    re_pred = 0;
    gpu_discard(&b, 1);
    CHECK(ndiscard == discards + 1, "a discard did not run with no predicate");
    re_owner = ~0u;
    int resets = re_resets;
    gpu_draw_states_reset();
    CHECK(re_resets == resets + 1, "a state load did not drop the render enable's decisions");
}

static void test_calls(void) {
    DrawOwner own = {0};
    DrawCall dc = {0};
    norder = 0;
    gpu_method_barrier(3);
    gpu_stream_incr(17);
    gpu_draw(&a, &dc, &own);
    gpu_samples_reset(0x80000001u);
    gpu_samples_enable(0x80000002u, 1);
    gpu_stream_run(1);
    gpu_predicate_drop(99);
    gpu_ready();
    gpu_stream_end();
    gq_drain();
    order[norder] = 0;
    CHECK(!strcmp(order, "bidsnupre"), "the calls reached the renderer as %s", order);
    static const u32 want[9][2] = {{3, 0}, {17, 0}, {0, 0}, {0x80000001u, 0}, {0x80000002u, 1}, {1, 0}, {99, 0},
                                   {0, 0}, {0, 0}};
    CHECK(norder == 9 && !memcmp(args, want, sizeof want), "a call's arguments did not arrive");
    CHECK(gpu_renderer_now() == g_renderer, "gpu_renderer_now is not the renderer");
    int seen = nseen_mem;
    gpu_mem_seen();
    gq_drain();
    CHECK(nseen_mem == seen + 1, "gpu_mem_seen did not reach the renderer's mem_seen");
}

/* ------------------------------------------------ gpu_split=1: the renderer thread runs the records */
static pthread_mutex_t gate_lk = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cv = PTHREAD_COND_INITIALIZER;
static int gate_open, gate_held;
/* a record that holds the renderer thread until the front end opens the gate */
static void held(const void* arg, u64 bytes) {
    (void)arg, (void)bytes;
    pthread_mutex_lock(&gate_lk);
    gate_held = 1;
    pthread_cond_broadcast(&gate_cv);
    while (!gate_open) pthread_cond_wait(&gate_cv, &gate_lk);
    pthread_mutex_unlock(&gate_lk);
}
static u8 split_seen[4];
static u32 split_n;
static pthread_t split_thread;
static void r_split_draw(const DsState* ds, const DrawCall* dc, DrawOwner* o) {
    (void)dc, (void)o;
    if (split_n < 4) split_seen[split_n++] = B(*ds, blend);
    split_thread = pthread_self();
}
static const Renderer split_renderer = {.name = "split", .draw = r_split_draw};
static int split_main(void) {
    DrawOwner own = {0};
    DrawCall dc = {0};
    g_settings.gpu_split = 1;
    g_renderer = &split_renderer;
    new_state(&a, 1, 0x10);
    gq_call(held, NULL, 0); /* the ring and the renderer thread start here */
    pthread_mutex_lock(&gate_lk);
    while (!gate_held) pthread_cond_wait(&gate_cv, &gate_lk);
    pthread_mutex_unlock(&gate_lk);
    /* the front end goes on while the renderer thread is held: each draw reads the state of its own time */
    gpu_draw(&a, &dc, &own);
    B(a, blend) = 0x20;
    a.gen[DS_BLEND] = gen_next++;
    gpu_draw(&a, &dc, &own);
    B(a, blend) = 0x30;
    a.gen[DS_BLEND] = gen_next++;
    CHECK(split_n == 0, "a draw ran while the renderer thread was held");
    pthread_mutex_lock(&gate_lk);
    gate_open = 1;
    pthread_cond_broadcast(&gate_cv);
    pthread_mutex_unlock(&gate_lk);
    gq_drain();
    CHECK(split_n == 2 && split_seen[0] == 0x10 && split_seen[1] == 0x20,
          "the renderer thread's draws read %u states: %#x %#x, not 0x10 0x20", split_n, split_seen[0], split_seen[1]);
    CHECK(!pthread_equal(split_thread, pthread_self()), "the draws ran on the front end's thread");
    if (failed) {
        fprintf(stderr, "gpu_records split: %d of %d checks failed\n", failed, checks);
        return 1;
    }
    printf("gpu_records split: %d checks passed\n", checks);
    return 0;
}

int main(int argc, char** argv) {
    /* the draws are set up on the renderer thread unless a setting turns it off */
    CHECK(g_settings.gpu_split == 2, "gpu_split is %d by default, not 2 (auto)", g_settings.gpu_split);
    if (argc > 1 && !strcmp(argv[1], "split")) return split_main();
    g_settings.gpu_split = 0; /* the records run on this thread, as they are queued */
    test_copy();
    test_dispatch();
    test_records();
    test_render_enable();
    test_inline_patches();
    test_calls();
    if (failed) {
        fprintf(stderr, "gpu_records: %d of %d checks failed\n", failed, checks);
        return 1;
    }
    printf("gpu_records: %d checks passed\n", checks);
    return 0;
}
