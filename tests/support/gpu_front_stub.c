/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* gpu/renderer.c's entry points of the front end, for a test that links a front end (maxwell3d.c, nvn_draw.c) with a
   stand-in renderer of its own and without gpu/renderer.c: each call goes straight to the test's g_renderer. The test
   keeps g_draws itself */
#include "nx/gpu/renderer.h"

void gpu_draw(const DsState* ds, const DrawCall* dc, DrawOwner* o) {
    if (g_renderer->draw) g_renderer->draw(ds, dc, o);
}
void gpu_clear(const DsState* ds, const DrawClear* c, const DrawOwner* o) {
    if (g_renderer->clear) g_renderer->clear(ds, c, o);
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
