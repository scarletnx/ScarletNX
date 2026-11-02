/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/video_core/engines/kepler_compute.cpp, src/video_core/engines/kepler_compute.h,
   src/video_core/engines/kepler_memory.cpp, src/video_core/engines/kepler_memory.h):
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project */
/* Native QMD scheduling follows Eglantine's src/video_core/native_qmd.h:
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-License-Identifier: GPL-3.0-or-later.
   PCAS actions and QMD 3.0 fields are from NVIDIA's clc7c0.h and clc7c0qmd.h. */
/* The compute engine (KEPLER_COMPUTE_B, 0xB1C0): launches, which the renderer runs from their launch description
   (the QMD, Eden's LaunchParams), its inline-to-memory methods and its report semaphore; and the inline-to-memory
   class (KEPLER_INLINE_TO_MEMORY_B, 0xA140, Eden's KeplerMemory). gpu.c's method_call hands both their methods, by
   the class bound to the call's subchannel. Register names are kepler_compute_regs.h's and
   inline_to_memory_regs.h's */
#include "nx/gpu/maxwell/engines_int.h"
#include "nx/gpu/maxwell/inline_to_memory_regs.h"
#include "nx/gpu/maxwell/kepler_compute_regs.h"
#include "common/utilities/endian.h"
#include "common/utilities/bits.h"
#include "nx/gpu/maxwell/draw_state.h"
#include "nx/gpu/shader/ampere.h"

#define QMD_BYTES (KEPLER_COMPUTE_QMD_WORDS * 4) /* the launch description, 256 bytes */
#define CAPTURE_CODE_BYTES 0x10000               /* a captured program: up to 64 KiB of it */
#define CAPTURE_PROGRAMS 64                      /* the programs captured at most */
/* renderer.h's method_barrier kind 5: the shader instruction cache is invalidated */
#define BARRIER_SHADER_INSTRUCTION_INVALIDATE 5

/* With diag=1 (the Shader captures setting), save the first CAPTURE_PROGRAMS compute programs and every failed
   native dispatch. A failure must remain available after the normal capture budget is full. */
static void capture(const u32* q, u64 code, int failed) {
    static u64 seen[CAPTURE_PROGRAMS];
    static u32 nseen;
    u64 pa = code + qmd_program_start(q);
    char path[sizeof g_exedir + 64];
    u8* buf;
    FILE* f;
    u32 i;
    if (!failed) {
        for (i = 0; i < nseen; i++)
            if (seen[i] == pa) return;
        if (nseen == CAPTURE_PROGRAMS) return;
        seen[nseen++] = pa;
    }
    buf = (u8*)malloc(QMD_BYTES + CAPTURE_CODE_BYTES);
    if (!buf) return;
    memcpy(buf, q, QMD_BYTES);
    snx_le32_array(buf, KEPLER_COMPUTE_QMD_WORDS); /* the description as guest memory held it */
    gmem_read_code(pa, buf + QMD_BYTES, CAPTURE_CODE_BYTES); /* up to where guest memory ends */
    u32 code_bytes = CAPTURE_CODE_BYTES;
    if (qmd_v3(q))
        for (;;) {
            u32 needed = shader_ampere_read_extent(buf + QMD_BYTES, code_bytes);
            if (needed <= code_bytes) {
                code_bytes = needed;
                break;
            }
            if (code_bytes == SHADER_AMPERE_NATIVE_MAX) break;
            u32 next = (u32)snx_align_up(needed, 0x1000);
            if (next < code_bytes * 2) next = code_bytes * 2;
            if (next > SHADER_AMPERE_NATIVE_MAX) next = SHADER_AMPERE_NATIVE_MAX;
            u8* grown = realloc(buf, QMD_BYTES + next);
            if (!grown) {
                free(buf);
                return;
            }
            buf = grown;
            gmem_read_code(pa + code_bytes, buf + QMD_BYTES + code_bytes, next - code_bytes);
            code_bytes = next;
        }
    snprintf(path, sizeof path, "%scaptures", g_exedir);
    plat_mkdir(path);
    snprintf(path, sizeof path, "%scaptures/compute", g_exedir);
    plat_mkdir(path);
    snprintf(path, sizeof path, "%scaptures/compute/cs_%010llx.bin", g_exedir, (unsigned long long)pa);
    f = fopen(path, "wb");
    if (f) {
        if (fwrite(buf, 1, QMD_BYTES + code_bytes, f) != QMD_BYTES + code_bytes)
            WARN_LIMIT(1, "gpu: %s wasn't written\n", path);
        fclose(f);
    }
    free(buf);
}
/* Eden's ProcessLaunch: the launch description at launch_desc_loc (in 256 byte units), the program at its
   program_start from code_loc. The renderer reads the description's fields (LaunchParams) */
static void launch(GpuChannel* ch) {
    const u32* r = ch->rcomp;
    u32 q[KEPLER_COMPUTE_QMD_WORDS];
    u64 code = ((u64)(r[KEPLER_COMPUTE_CODE_LOC_HIGH] & KEPLER_COMPUTE_CODE_LOC_HIGH_MASK) << 32) |
               r[KEPLER_COMPUTE_CODE_LOC_LOW];
    gmem_read((u64)r[KEPLER_COMPUTE_LAUNCH_DESC_LOC] << KEPLER_COMPUTE_LAUNCH_DESC_LOC_SHIFT, q, sizeof q);
    snx_le32_array(q, KEPLER_COMPUTE_QMD_WORDS); /* the renderer reads its words and bitfields as numbers */
    if (g_settings.shader_captures) capture(q, code, 0);
    if (!g_renderer->dispatch) return;
    DispatchState d;
    mx_dispatch_state(q, r, code, &d);
    g_renderer->dispatch(&d);
}
GpuQmd* gpu_qmd_append(GpuQmdCache* cache) {
    if (cache->count == cache->capacity) {
        u32 capacity = cache->capacity ? cache->capacity * 2 : 32;
        GpuQmd* entries = realloc(cache->entries, (size_t)capacity * sizeof *entries);
        if (!entries) return NULL;
        cache->entries = entries;
        cache->capacity = capacity;
    }
    GpuQmd* entry = &cache->entries[cache->count++];
    *entry = (GpuQmd){0};
    return entry;
}
static int qmd_error(u64 addr, const char* reason) {
    WARN_LIMIT(32, "gpu: native QMD %llx: %s\n", (unsigned long long)addr, reason);
    return 0;
}
static int qmd_read(u64 addr, u32 q[KEPLER_COMPUTE_QMD_WORDS]) {
    if (gmem_read(addr, q, QMD_BYTES)) return qmd_error(addr, "unmapped descriptor");
    snx_le32_array(q, KEPLER_COMPUTE_QMD_WORDS);
    if (!qmd_v3(q) || (q[4] & (1u << 8))) return qmd_error(addr, "unsupported version or queue");
    return 1;
}
/* A decrement can precede its schedule, including one from another channel. Keep it until the descriptor is copied.
   INVALIDATE alone needs no action here: we retain counters, not a cached copy of the guest's descriptor bytes. */
static int qmd_action(GpuChannel* ch, u64 addr, u32 action, u32 count) {
    if (action == 0 || action == 1) return 1;
    if (action != 2 && action != 3 && action != 7 && action != 12)
        return qmd_error(addr, "unsupported PCAS action");
    GpuQmdCache* cache = &g_gpu.qmd;
    u32 i;
    for (i = 0; i < cache->count && cache->entries[i].addr != addr; ++i) {}
    GpuQmd* entry = i < cache->count ? &cache->entries[i] : gpu_qmd_append(cache);
    if (!entry) fatal("gpu: out of memory for QMD counters\n");
    entry->addr = addr;
    if (action == 7 || action == 12) {
        u32 delta = action == 7 ? 0u - count : count;
        if (entry->status == GPU_QMD_SCHEDULED)
            entry->dependencies += delta;
        else
            entry->deferred += delta;
    } else if (action == 3 || entry->status == GPU_QMD_EMPTY) {
        u32 q[KEPLER_COMPUTE_QMD_WORDS];
        if (!qmd_read(addr, q)) return 0;
        entry->dependencies = ((q[4] >> 14) & 0xFFFFu) + entry->deferred;
        entry->deferred = 0;
        entry->status = GPU_QMD_SCHEDULED;
        entry->channel = ch->id;
    }
    return 1;
}
/* A PCAS action on the QMD at addr, then every QMD it leaves ready runs. NVN's compute QueueFenceSync macro patches
   and submits a zero-grid QMD. Its release writes the sync's first-page semaphore directly. The second-page stream
   releases are a different timeline, not an alias. */
static void native_pcas(GpuChannel* ch, u64 addr, u32 action, u32 count) {
    if (!qmd_action(ch, addr, action, count)) return;
    /* Completing one QMD can make an earlier entry ready. Iterate instead of recursing through dependency chains. */
    u32 steps = 0;
    int progress;
    do {
        progress = 0;
        for (u32 i = 0; i < g_gpu.qmd.count; ++i) {
            GpuQmd* entry = &g_gpu.qmd.entries[i];
            if (entry->status != GPU_QMD_SCHEDULED || (i32)entry->dependencies > 0) continue;
            addr = entry->addr;
            if (entry->dependencies) {
                qmd_error(addr, "dependency counter underflow");
                return;
            }
            if (++steps > MME_STEPS) {
                qmd_error(addr, "dependency chain limit");
                return;
            }
            u32 q[KEPLER_COMPUTE_QMD_WORDS];
            if (!qmd_read(addr, q)) return;
            /* Validate all releases before dispatch or completion: a trap, a three-word structure, or a reduction on
               the four-word one (a payload and its timestamp) has no meaning here */
            static const u32 releases[] = {24, 28, 52};
            for (u32 slot = 0; slot < 3; ++slot) {
                u32 flags = q[releases[slot] + 1];
                if ((flags & (1u << 23)) &&
                    ((flags & (3u << 27)) || (flags >> 30) == 3 || ((flags & (1u << 26)) && !(flags >> 30)) ||
                     ((flags & (1u << 26)) && ((flags >> 24) & 3) > 1))) {
                    qmd_error(addr, "unsupported release trap, size or reduction format");
                    return;
                }
            }
            if (q[16] & (1u << 5)) {
                qmd_error(addr, "unsupported second dependent QMD");
                return;
            }
            u32 action = q[16] >> 1 & 7;
            if ((q[16] & 1) && action != 1 && action != 3 && action != 4) {
                qmd_error(addr, "unsupported dependent QMD action");
                return;
            }
            GpuChannel* owner = g_gpu.chan[entry->channel];
            if (qmd_grid_dim_x(q) && qmd_grid_dim_y(q) && qmd_grid_dim_z(q)) {
                u64 code = (u64)(q[49] & 0x1FFFFu) << 32 | q[48];
                if (g_settings.shader_captures) capture(q, code, 0);
                DispatchState d;
                if (owner) mx_dispatch_state(q, owner->rcomp, code, &d);
                if (!owner || !g_renderer->dispatch || !g_renderer->dispatch(&d)) {
                    if (g_settings.shader_captures) capture(q, code, 1);
                    fatal("gpu: native QMD %llx: compute dispatch failed, its releases were not executed\n",
                          (unsigned long long)addr);
                }
            }
            entry->status = GPU_QMD_COMPLETE;
            for (u32 slot = 0; slot < 3; ++slot) {
                u32 base = releases[slot], flags = q[base + 1];
                if (!(flags & (1u << 23))) continue;
                u64 destination = (u64)(flags & 0xFFu) << 32 | q[base];
                u64 payload = q[base + 2];
                if (flags & (1u << 29)) payload |= (u64)q[base + 3] << 32;
                u32 size = flags >> 30;
                if (flags & (1u << 26)) { /* REDUCTION_ENABLE: OP 22:20, FORMAT 25:24 (bit 24 signed) */
                    gpu_release_reduce(destination, flags >> 20 & 7, (int)(flags >> 24 & 1), size == 2 ? 8 : 4,
                                       payload);
                    continue;
                }
                u8 bytes[16];
                snx_st_le64(bytes, payload);
                if (!size) snx_st_le64(bytes + 8, gpu_ticks());
                gpu_release_write(destination, bytes, size == 1 ? 4 : size == 2 ? 8 : 16);
            }
            /* qmd_action can grow the shared table. Do not retain entry across this call. */
            if ((q[16] & 1) &&
                !qmd_action(owner, (u64)q[15] << 8, action == 4 ? 7 : action == 3 ? 3 : 2, 1))
                return;
            progress = 1;
        }
    } while (progress);
}
/* LOAD_INLINE_QMD_DATA's last word: the QMD goes to the address SET_INLINE_QMD_ADDRESS gave, and is scheduled */
static void inline_qmd(GpuChannel* ch) {
    const u32* r = ch->rcomp;
    u64 addr = ((u64)r[KEPLER_COMPUTE_INLINE_QMD_ADDRESS] << 32 | r[KEPLER_COMPUTE_INLINE_QMD_ADDRESS + 1]) << 8;
    u32 q[KEPLER_COMPUTE_QMD_WORDS];
    memcpy(q, &r[KEPLER_COMPUTE_INLINE_QMD_DATA], sizeof q);
    snx_le32_array(q, KEPLER_COMPUTE_QMD_WORDS); /* guest bytes */
    if (gmem_write(addr, q, QMD_BYTES)) {
        qmd_error(addr, "unmapped inline descriptor");
        return;
    }
    /* REQUIRE_SCHEDULING_PCAS defers execution unless SEND_PCAS_ENABLE requests an inline schedule. */
    if (!(r[KEPLER_COMPUTE_INLINE_QMD_DATA + 4] & (1u << 12)) ||
        (r[KEPLER_COMPUTE_QMD_VIRTUALIZATION_CONTROL] & (1u << 24)))
        native_pcas(ch, addr, 3, 1); /* INVALIDATE_COPY_SCHEDULE */
}
/* Eden's KeplerCompute::CallMethod: every method is kept in the registers, and these act */
void compute_method(GpuChannel* ch, u32 m, u32 v, int native) {
    m &= KEPLER_COMPUTE_METHODS - 1;
    ch->rcomp[m] = v;
    switch (m) {
        case KEPLER_COMPUTE_EXEC_UPLOAD: i2m_launch(ch, ch->rcomp, KEPLER_COMPUTE_UPLOAD); break;
        case KEPLER_COMPUTE_DATA_UPLOAD: i2m_data(ch, v); break;
        case KEPLER_COMPUTE_INVALIDATE_SHADER_CACHES:
        case KEPLER_COMPUTE_INVALIDATE_SHADER_CACHES_NO_WFI:
            if ((v & KEPLER_COMPUTE_INVALIDATE_INSTRUCTION) && g_renderer->method_barrier)
                g_renderer->method_barrier(BARRIER_SHADER_INSTRUCTION_INVALIDATE);
            break;
        case KEPLER_COMPUTE_LAUNCH:
            if (native) {
                if (v & 2) native_pcas(ch, (u64)ch->rcomp[KEPLER_COMPUTE_LAUNCH_DESC_LOC] << 8, v & 3, 1);
            } else
                launch(ch);
            break;
        case KEPLER_COMPUTE_LAUNCH_PCAS2:
            if (native)
                native_pcas(ch, (u64)ch->rcomp[KEPLER_COMPUTE_LAUNCH_DESC_LOC] << 8, v & 15,
                            ((v >> 14) & 0x3FFu) + 1);
            break;
        case KEPLER_COMPUTE_INLINE_QMD_DATA + KEPLER_COMPUTE_QMD_WORDS - 1:
            if (native) inline_qmd(ch);
            break;
        case KEPLER_COMPUTE_REPORT_SEMAPHORE_QUERY: /* the report semaphore, the 3D engine's */
            query_get(ch, ch->rcomp, KEPLER_COMPUTE_REPORT_SEMAPHORE, v);
            break;
        default: break;
    }
}
/* Eden's KeplerMemory::CallMethod: the inline-to-memory class is only the upload block */
void kepler_i2m_method(GpuChannel* ch, u32 m, u32 v) {
    if (m >= KEPLER_I2M_METHODS) return;
    ch->ri2m[m] = v;
    if (m == I2M_EXEC_UPLOAD)
        i2m_launch(ch, ch->ri2m, I2M_UPLOAD_REGS);
    else if (m == I2M_DATA_UPLOAD)
        i2m_data(ch, v);
}
