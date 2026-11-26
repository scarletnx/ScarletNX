/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A test that includes a runtime source file which emits zones sees SNX_TRACY (snx_nx_flags) and links
   Tracy::TracyClient, but not the runtime's tracy.c: the gate is gated out here. */
#include "common/utilities/tracy.h"
#if defined(SNX_TRACY)
_Atomic int snx_tracy_memory_detail;
_Atomic int snx_tracy_memory_detail_ever;
_Atomic int snx_tracy_frame_images_live;
_Atomic int snx_tracy_zone_callstack;
SnxTracySection snx_tracy_section_start_c(const char* name, uint16_t category) {
    (void)name, (void)category;
    return (SnxTracySection){0, 0};
}
void snx_tracy_section_setup(uint16_t category, const char* name) { (void)category, (void)name; }
void snx_tracy_frame_mark_named(const char* name) { (void)name; }
void snx_tracy_source_add(const char* name, const char* bytes, size_t size) {
    (void)name, (void)bytes, (void)size;
}
void snx_tracy_thread_name_hint(const char* name, const char* hint) { (void)name, (void)hint; }
int snx_tracy_enter(void) { return 0; }
void snx_tracy_leave(void) {}
void snx_tracy_thread_name(const char* name) { (void)name; }
TracyCZoneCtx snx_tracy_zone_begin(const struct ___tracy_source_location_data* srcloc) {
    TracyCZoneCtx ctx = {0};
    (void)srcloc;
    return ctx;
}
TracyCZoneCtx snx_tracy_zone_begin_cs(const struct ___tracy_source_location_data* srcloc, int depth) {
    (void)depth;
    return snx_tracy_zone_begin(srcloc);
}
void snx_tracy_plot_cfg(const char* name, int type, int step, int fill, uint32_t color) {
    (void)name, (void)type, (void)step, (void)fill, (void)color;
}
void snx_tracy_frame_mark(void) {}
int snx_tracy_attached(void) { return 0; }
uint64_t snx_tracy_connection_id(void) { return 1; }
void snx_tracy_zone_end(TracyCZoneCtx ctx) { (void)ctx; }
TracyCZoneCtx snx_tracy_zone_begin_transient(const char* name, int callstack, int depth) {
    TracyCZoneCtx ctx = {0};
    (void)name, (void)callstack, (void)depth;
    return ctx;
}
void snx_tracy_zone_end_transient(TracyCZoneCtx ctx) { (void)ctx; }
void snx_tracy_fiber_enter(uint32_t slot, uint32_t kind, const char* name) {
    (void)slot, (void)kind, (void)name;
}
void snx_tracy_fiber_leave(void) {}
void snx_tracy_fiber_leave_all(void) {}
/* The lifetime and message calls tracy.c owns: this build has no client, so they consume their arguments and do
   nothing. The header declares them for a SNX_TRACY build, which does not link tracy.c. */
int snx_tracy_init(const char* title, const SnxTracyHost* host) {
    (void)title, (void)host;
    return 0;
}
int snx_tracy_console(const char* folder, const char* name, int headless) {
    (void)folder, (void)name, (void)headless;
    return 0;
}
void snx_tracy_capture_now(void) {}
void snx_tracy_log(int lvl, const char* text) {
    (void)lvl, (void)text;
}
void snx_tracy_fatal(const char* text) { (void)text; }
void snx_tracy_shutdown(void) {}
/* The settings gates read once at startup: no client here, so they stay off and never open a hot-path call. */
_Atomic int snx_tracy_memory_callstack;
_Atomic int snx_tracy_lock_marks;
_Atomic int snx_tracy_gpu_zone_callstack;
/* The phase sections and lock marks: consume their arguments and do nothing, as the section's scope cleanup expects. */
SnxTracySection snx_tracy_section_start(const char* name) {
    SnxTracySection section = {0};
    (void)name;
    return section;
}
void snx_tracy_section_at_scope(SnxTracySection* s) { (void)s; }
void snx_tracy_lock_mark(SnxTracyLock* l, const char* file, uint32_t line) {
    (void)l, (void)file, (void)line;
}
#endif
