/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The renderer state the GPU front end and the renderer share (renderer.h) */
#include "nx/gpu/renderer.h"

int g_renderer_up;
u64 g_draws;
u64 g_tex_cache_hits, g_tex_cache_misses, g_tex_upload_bytes, g_tex_frees;
/* set by the renderer while it holds results not yet in guest memory, read by every reader of guest memory: a reader
   that sees 0 has nothing to land. The compute and graphics sides set theirs from different threads: a cache line
   each */
_Alignas(64) _Atomic int g_cs_any;
_Alignas(64) _Atomic int g_gfx_any;
_Alignas(64) _Atomic int g_uma_any;
/* The GPU mirror's programs' writes that guest memory doesn't have yet (gpu/vk/mirror.c), a flag of their own: they
   are not graphics results (g_gfx_any is buffer.c's alone), and with none of the others set a read of guest memory
   under them asked for nothing and read the bytes from before the writes. One game's particle programs keep their
   allocation counter there; a dispatch that could not take the mirror read the counter in guest memory while the
   dispatch before it had written it in the mirror, and handed out the same records twice */
_Alignas(64) _Atomic int g_mir_any;
