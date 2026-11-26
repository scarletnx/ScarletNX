/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The one piece of the C++ client the C API keeps private: the connection's id, and whether a server is attached.
   A GPU emission's epoch is keyed on it (tracy.c's GPU track): a new connection replays the deferred contexts,
   so its events may not be mixed with the last connection's. TracyCIsConnected reads the client's own data,
   which a run that never started one has none of, so the C gate decides whether to reach it at all.
   This file also holds the calls the C API cannot make itself: the client's program name, the thread group hint,
   the trace parameters and the source-contents callback. Their callbacks and registry live here so no C file
   has to include a C++ header. */
extern "C" {
#include "common/utilities/tracy.h"
}
#include "common/utilities/bits.h"
#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <cstddef>
#include <mutex>

#include "common/TracySystem.hpp"
#include <tracy/Tracy.hpp>

/* tracy.h declares these globals. Repeat the declarations here so the bridge stays self-contained: the keyword
   spelling `_Atomic int` is the C type, while <stdatomic.h>'s `_Atomic(int)` macro would name std::atomic<int> in
   C++ and conflict with it. Clang's __c11_atomic_* builtins are the accessors for this C atomic type. */
extern "C" _Atomic int snx_tracy_frame_images_live;
extern "C" _Atomic int snx_tracy_memory_detail;
extern "C" _Atomic int snx_tracy_memory_callstack;
extern "C" _Atomic int snx_tracy_zone_callstack;
extern "C" _Atomic int snx_tracy_gpu_zone_callstack;
extern "C" _Atomic int snx_tracy_lock_marks;

extern "C" uint64_t snx_tracy_connection_id(void) {
#ifdef TRACY_ON_DEMAND
    uint64_t id = tracy::GetProfiler().ConnectionId();
    return id ? id : 1; /* the client numbers connections from 0; the API and its stub use 1 for the first one */
#else
    return 1;
#endif
}

extern "C" int snx_tracy_attached(void) {
    if (!snx_tracy_gate_open()) return 0;
    return TracyIsConnected;
}

extern "C" void snx_tracy_conn_set_program_name(const char* name) {
    if (name) TracySetProgramName(name);
}

/* SetThreadNameWithHint takes an int32_t group hint, not a string (TracySystem.hpp:36). A decimal string is the
   hint itself; a symbolic one is a stable FNV-1a key, so two names of one group sort together in the GUI. */
static int32_t snx_tracy_group_hint(const char* hint) {
    if (!hint || !*hint) return 0;
    return snx_fnv1a32(SNX_FNV32_BASIS, hint, strlen(hint));
}

extern "C" void snx_tracy_conn_thread_name_hint(const char* name, const char* hint) {
    if (!name || !*name) return;
    tracy::SetThreadNameWithHint(name, snx_tracy_group_hint(hint));
}

/* The parameter indices are the ones passed to ParameterSetup; the server echoes an index in the callback. */
enum {
    SNX_PARAM_FRAME_IMAGE_SECONDS = 0,
    SNX_PARAM_MEMORY_DETAIL = 1,
    SNX_PARAM_ZONE_CALLSTACK = 2,
    SNX_PARAM_GPU_ZONE_CALLSTACK = 3,
    SNX_PARAM_LOCK_MARKS = 4,
    SNX_PARAM_MEMORY_CALLSTACK = 5
};

/* Tracy hands the server a depth as a signed int; the server walks at most 16 frames, so a larger value is useless
   and a negative one would wrap in its loop. Clamp at the bridge, the one place that writes these atomics. */
static int32_t snx_tracy_param_depth(int32_t val) {
    if (val < 0) return 0;
    if (val > 16) return 16;
    return val;
}

static void snx_tracy_parameter_changed(void* data, uint32_t idx, int32_t val) {
    (void)data;
    switch (idx) {
    case SNX_PARAM_FRAME_IMAGE_SECONDS:
        /* The renderer reads this as u32 seconds; a negative value would become a centuries-long interval. */
        if (val < 0) val = 0;
        if (val > 60) val = 60;
        __c11_atomic_store(&snx_tracy_frame_images_live, val, __ATOMIC_RELAXED);
        break;
    case SNX_PARAM_MEMORY_DETAIL:
        __c11_atomic_store(&snx_tracy_memory_detail, val != 0, __ATOMIC_RELAXED);
        if (val != 0) __c11_atomic_store(&snx_tracy_memory_detail_ever, 1, __ATOMIC_RELAXED);
        break;
    case SNX_PARAM_ZONE_CALLSTACK:
        __c11_atomic_store(&snx_tracy_zone_callstack, snx_tracy_param_depth(val), __ATOMIC_RELAXED);
        break;
    case SNX_PARAM_GPU_ZONE_CALLSTACK:
        __c11_atomic_store(&snx_tracy_gpu_zone_callstack, snx_tracy_param_depth(val), __ATOMIC_RELAXED);
        break;
    case SNX_PARAM_LOCK_MARKS:
        __c11_atomic_store(&snx_tracy_lock_marks, val != 0, __ATOMIC_RELAXED);
        break;
    case SNX_PARAM_MEMORY_CALLSTACK:
        __c11_atomic_store(&snx_tracy_memory_callstack, snx_tracy_param_depth(val), __ATOMIC_RELAXED);
        break;
    default:
        break;
    }
}

extern "C" void snx_tracy_conn_register_parameters(void) {
    TracyParameterRegister(snx_tracy_parameter_changed, NULL);
    TracyParameterSetup(SNX_PARAM_FRAME_IMAGE_SECONDS, "Frame image seconds", TracyParamTypeInt,
                        (int32_t)__c11_atomic_load(&snx_tracy_frame_images_live, __ATOMIC_RELAXED));
    TracyParameterSetup(SNX_PARAM_MEMORY_DETAIL, "Memory detail", TracyParamTypeBool,
                        __c11_atomic_load(&snx_tracy_memory_detail, __ATOMIC_RELAXED) != 0);
    TracyParameterSetup(SNX_PARAM_ZONE_CALLSTACK, "Zone callstack depth", TracyParamTypeInt,
                        __c11_atomic_load(&snx_tracy_zone_callstack, __ATOMIC_RELAXED));
    TracyParameterSetup(SNX_PARAM_GPU_ZONE_CALLSTACK, "GPU zone callstack depth", TracyParamTypeInt,
                        __c11_atomic_load(&snx_tracy_gpu_zone_callstack, __ATOMIC_RELAXED));
    TracyParameterSetup(SNX_PARAM_LOCK_MARKS, "Lock marks", TracyParamTypeBool,
                        __c11_atomic_load(&snx_tracy_lock_marks, __ATOMIC_RELAXED) != 0);
    TracyParameterSetup(SNX_PARAM_MEMORY_CALLSTACK, "Memory callstack depth", TracyParamTypeInt,
                        __c11_atomic_load(&snx_tracy_memory_callstack, __ATOMIC_RELAXED));
}

namespace {

const std::size_t SNX_SOURCE_MAX = 32;

struct SourceEntry {
    char* name;
    char* bytes;
    std::size_t size;
};

/* The add hook can run on a guest thread and the lookup on Tracy's network thread. The mutex puts their access
   in order; a name is never changed after it is stored, so a lookup that finds one copies it under the same lock. */
struct SourceState {
    std::mutex lock;
    SourceEntry entries[SNX_SOURCE_MAX];
    std::size_t count;
};
static SourceState g_sources_state;
#define g_source_lock  (g_sources_state.lock)
#define g_sources      (g_sources_state.entries)
#define g_source_count (g_sources_state.count)

} // namespace

extern "C" void snx_tracy_source_add(const char* name, const char* bytes, size_t size) {
    if (!name || !*name || !bytes || !size) return;
    const std::size_t name_size = strlen(name) + 1;
    char* name_copy = (char*)malloc(name_size);
    if (!name_copy) return;
    char* bytes_copy = (char*)tracy::tracy_malloc_fast(size);
    if (!bytes_copy) {
        free(name_copy);
        return;
    }
    memcpy(name_copy, name, name_size);
    memcpy(bytes_copy, bytes, size);

    std::lock_guard<std::mutex> lock(g_source_lock);
    for (std::size_t i = 0; i < g_source_count; ++i) {
        if (strcmp(g_sources[i].name, name) != 0) continue;
        /* A replacement's old buffers came from these allocators; free them before the entry changes. */
        tracy::tracy_free_fast(g_sources[i].bytes);
        free(g_sources[i].name);
        g_sources[i].name = name_copy;
        g_sources[i].bytes = bytes_copy;
        g_sources[i].size = size;
        return;
    }
    if (g_source_count == SNX_SOURCE_MAX) { /* bounded: a later source does not push out one already announced */
        free(name_copy);
        tracy::tracy_free_fast(bytes_copy);
        return;
    }
    g_sources[g_source_count].name = name_copy;
    g_sources[g_source_count].bytes = bytes_copy;
    g_sources[g_source_count].size = size;
    ++g_source_count;
}

/* Exact SourceContentsCallback signature (TracyProfiler.hpp:220): the size is an out parameter. Tracy frees the
   returned buffer with tracy_free_fast after it sends the contents (TracyProfiler.cpp:2956), so the callback
   must not free a buffer it returned earlier: a second query can arrive before the first one is flushed. */
static char* snx_tracy_source_lookup(void* data, const char* filename, size_t& size) {
    (void)data;
    if (!filename) return NULL;
    std::lock_guard<std::mutex> lock(g_source_lock);
    for (std::size_t i = 0; i < g_source_count; ++i) {
        if (strcmp(g_sources[i].name, filename) != 0) continue;
        const std::size_t n = g_sources[i].size;
        char* out = (char*)tracy::tracy_malloc_fast(n ? n : 1);
        if (!out) return NULL;
        memcpy(out, g_sources[i].bytes, n);
        size = n;
        return out;
    }
    return NULL;
}

extern "C" void snx_tracy_conn_register_source_callback(void) {
    TracySourceCallbackRegister(snx_tracy_source_lookup, NULL);
}
