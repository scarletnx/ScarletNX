/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The renderer state the GPU front end and the renderer share (renderer.h) */
#include "nx/gpu/renderer.h"

int g_renderer_up;
u64 g_draws;
/* set by the renderer while it holds results not yet in guest memory, read by every reader of guest memory: a reader
   that sees 0 has nothing to land. The compute and graphics sides set theirs from different threads: a cache line
   each */
_Alignas(64) _Atomic int g_cs_any;
_Alignas(64) _Atomic int g_gfx_any;
_Alignas(64) _Atomic int g_uma_any;
