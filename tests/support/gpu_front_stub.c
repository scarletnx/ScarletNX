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
void gpu_mem_seen(void) {
    if (g_renderer->mem_seen) g_renderer->mem_seen();
    else if (g_renderer->mem_may_have_changed) g_renderer->mem_may_have_changed();
}
void gpu_xfb_control(const DsState* ds, DrawOwner* o, u64 control, u32 what, u32 stride) {
    if (g_renderer->xfb_control) g_renderer->xfb_control(ds->owner, control, what, stride, o);
}
/* a test that checks the inline ranges a front end names sets this to its own recorder */
extern void (*g_stub_inline_changed)(const DsState* ds, u64 before, u32 off, u32 len);
void (*g_stub_inline_changed)(const DsState* ds, u64 before, u32 off, u32 len);
void gpu_ds_inline_changed(const DsState* ds, u64 before, u32 off, u32 len) {
    if (g_stub_inline_changed) g_stub_inline_changed(ds, before, off, len);
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
/* the calls that waited for the queue (gpu_renderer_now): what the front end runs on the renderer's side itself */
extern int g_front_stub_waits;
int g_front_stub_waits;
const Renderer* gpu_renderer_now_at(const char* file, int line) {
    (void)file, (void)line;
    g_front_stub_waits++;
    return g_renderer;
}
/* no increments wait in a queue here: the renderer's count is the value */
u32 gpu_release_value(u32 id) { return g_renderer->release_value ? g_renderer->release_value(id) : 0; }
u32 gpu_shading_rate(const GpuShadingRate* r) { return g_renderer->shading_rate ? g_renderer->shading_rate(r) : 0; }
#ifndef SNX_TEST_GPU_QUEUE /* gpu.c's statistics of inline data, for a test that doesn't link gpu.c */
GpuInlineMark gpu_inline_begin(void) { return (GpuInlineMark){0}; }
void gpu_inline_end(int kind, u64 bytes, GpuInlineMark m) {
    (void)kind;
    (void)bytes;
    (void)m;
}
#endif
/* gpu_queue.c's call record, for a test that doesn't link it (SNX_TEST_GPU_QUEUE says one does): the call runs at
   once, as the queue runs it without gpu_split */
#ifndef SNX_TEST_GPU_QUEUE
void gq_call(GqFn fn, const void* arg, u64 bytes) { fn(arg, bytes); }
int gq_queues(void) { return 0; } /* no renderer thread: the front end calls the renderer itself */
void gq_call_writes(GqFn fn, const void* arg, u64 bytes, const GqRange* ranges, u32 n) {
    (void)ranges;
    (void)n;
    fn(arg, bytes);
}
void gq_read_wait(u64 gva, u64 size) { /* nothing queued */
    (void)gva;
    (void)size;
}
#endif
