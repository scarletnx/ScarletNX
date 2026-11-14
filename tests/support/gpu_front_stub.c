/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* gpu/renderer.c's entry points of the front end, for a test that links a front end (maxwell3d.c, nvn_draw.c) with a
   stand-in renderer of its own and without gpu/renderer.c: each call goes straight to the test's g_renderer. The test
   keeps g_draws itself */
#include "nx/gpu/renderer.h"
#include "nx/gpu/gpu_queue.h"

/* a test that links maxwell/engines.c sets this to its gpu_render_state: the render enable's decision, which the
   renderer's side applies to the draws and clears (gpu/renderer.c ds_run) */
extern int (*g_stub_render_state)(u32 owner, u32* pred);
int (*g_stub_render_state)(u32 owner, u32* pred);
static int stub_off(const DsState* ds) {
    u32 pred = 0;
    return g_stub_render_state && g_stub_render_state(ds->owner, &pred) == 1;
}
void gpu_draw(const DsState* ds, const DrawCall* dc, DrawOwner* o) {
    if (g_renderer->draw && !stub_off(ds)) g_renderer->draw(ds, dc, o);
}
void gpu_clear(const DsState* ds, const DrawClear* c, const DrawOwner* o) {
    if (g_renderer->clear && !stub_off(ds)) g_renderer->clear(ds, c, o);
}
void gpu_draw_texture(const DsState* ds, const DrawTexture* t) {
    if (g_renderer->draw_texture) g_renderer->draw_texture(ds, t);
}
void gpu_discard(const DsState* ds, u32 targets) {
    if (g_renderer->discard) g_renderer->discard(ds, targets);
}
void gpu_dispatch(const DispatchState* d) {
    if (g_renderer->dispatch) g_renderer->dispatch(d);
}
void gpu_draw_states_reset(void) {}
void gpu_method_barrier(int kind) {
    if (g_renderer->method_barrier) g_renderer->method_barrier(kind);
}
void gpu_stream_incr(u32 id) {
    if (g_renderer->stream_incr) g_renderer->stream_incr(id);
}
void gpu_stream_end(void) {
    if (g_renderer->stream_end) g_renderer->stream_end();
}
void gpu_ready(void) {
    if (g_renderer->ready) g_renderer->ready();
}
void gpu_stream_run(int on) {
    if (g_renderer->stream_run) g_renderer->stream_run(on);
}
void gpu_samples_reset(u32 owner) {
    if (g_renderer->samples_reset) g_renderer->samples_reset(owner);
}
void gpu_samples_enable(u32 owner, int on) {
    if (g_renderer->samples_enable) g_renderer->samples_enable(owner, on);
}
void gpu_predicate_drop(u32 pred) {
    if (g_renderer->render_predicate_drop) g_renderer->render_predicate_drop(pred);
}
const Renderer* gpu_renderer_now(void) { return g_renderer; }
/* gpu_queue.c's call record, which this test doesn't link: the call runs at once, as the queue runs it without
   gpu_split */
void gq_call(GqFn fn, const void* arg, u64 bytes) { fn(arg, bytes); }
