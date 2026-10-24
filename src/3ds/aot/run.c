/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "3ds/aot/ctr_module.h"
#include "3ds/aot/ctr_compile.h"
#include "3ds/aot/ctr_jit.h"
#include "3ds/aot/ctr_discover.h"
#include "3ds/memory/host.h"
#include "common/utilities/common.h"
#include "common/utilities/game_ini.h"
#include "3ds/runtime/internal.h"
#include "3ds/gpu/present.h"
#include "common/utilities/build_info.h"
#include "3ds/audio/output.h"
#include "3ds/input/input.h"
#include "common/filesystem/host.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SNX_CTR_RUN_SLICE 1000000u

/* whose an event is: a service's that signals it, or the game's own */
static const char* event_owner(const SnxCtrProcess* p, uint32_t id) {
    if (id == p->apt_notification) return "APT's notification";
    if (id == p->apt_parameter) return "APT's parameter";
    for (unsigned i = 0; i < sizeof p->hid_objects / sizeof *p->hid_objects; ++i)
        if (id == p->hid_objects[i]) return "HID's";
    for (unsigned i = 0; i < CTR_GSP_THREADS; ++i)
        if (id == p->gsp.event_object[i]) return "GSP's interrupt";
    for (unsigned i = 0; i < sizeof p->dsp.interrupt / sizeof *p->dsp.interrupt; ++i)
        if (id == p->dsp.interrupt[i]) return "the DSP's interrupt";
    if (id == p->dsp.semaphore) return "the DSP's semaphore";
    return p->objects[id].from_app ? "the game's" : "a service's";
}
/* The return addresses on a thread's stack, as a backtrace: the words that point just past a BL or BLX (ARM, or with
   the low bit set Thumb's BL/BLX suffix) into the process's text, nearest first */
static void report_stack(SnxCtrProcess* p, const SnxArmCPU* cpu) {
    const SnxCtrSegment* text = &p->segment[0];
    unsigned shown = 0;
    printf("    stack:");
    for (uint32_t at = cpu->r[13] & ~3u; shown < 12 && at - (cpu->r[13] & ~3u) < 0x2000; at += 4) {
        unsigned char b[4];
        if (snx_ctr_read_ram(p->space, at, b, 4) != SNX_CTR_MEM_OK) break;
        uint32_t w = (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
        bool thumb = w & 1;
        uint32_t ret = w & ~1u;
        if (ret < text->address + 4 || ret >= text->address + text->bytes || (!thumb && (ret & 3))) continue;
        unsigned char c[4];
        if (snx_ctr_read_ram(p->space, ret - (thumb ? 2 : 4), c, thumb ? 2 : 4) != SNX_CTR_MEM_OK) continue;
        uint32_t i = (uint32_t)c[0] | (uint32_t)c[1] << 8 | (thumb ? 0 : (uint32_t)c[2] << 16 | (uint32_t)c[3] << 24);
        bool call = thumb ? (i & 0xe800) == 0xe800 /* BL or BLX suffix */
                          : (i & 0x0f000000) == 0x0b000000 || (i & 0xfe000000) == 0xfa000000 ||
                                (i & 0x0ffffff0) == 0x012fff30;
        if (!call) continue;
        printf(" 0x%08" PRIx32, w);
        ++shown;
    }
    putchar('\n');
}
/* the flat space's fault resolver (memory/host.h): write tracking's */
static bool track_fault(void* context, uint32_t va) { return snx_ctr_track_fault(context, va); }
/* Every live thread, where it is and what it waits on, as the run ended */
static void report_threads(SnxCtrProcess* p) {
    static const char* const types[] = {"free",  "process", "thread",    "arbiter", "session",       "mutex",
                                        "event", "shared",  "file",      "semaphore", "timer", "resource limit"};
    for (uint32_t i = 1; i < CTR_OBJECTS; ++i) {
        const CtrObject* o = &p->objects[i];
        if (o->type != CTR_OBJ_THREAD || o->data.thread.dead) continue;
        const CtrThread* t = &o->data.thread;
        const SnxArmCPU* cpu = i == p->current_thread ? &p->cpu.state.cpu : &t->context;
        printf("  thread %" PRIu32 "%s: core %" PRIu32 " priority 0x%02" PRIx32 " pc 0x%08" PRIx32 " lr 0x%08" PRIx32 " tls 0x%08" PRIx32,
               i, i == p->current_thread ? " (running)" : "", t->core, t->priority, cpu->r[15], cpu->r[14],
               cpu->tpidruro);
        if (t->waiting_object) {
            const CtrObject* w = &p->objects[t->waiting_object];
            printf(", waits on %s %" PRIu32, w->type < sizeof types / sizeof *types ? types[w->type] : "?",
                   t->waiting_object);
            if (w->type == CTR_OBJ_EVENT) printf(" (%s)", event_owner(p, t->waiting_object));
            for (uint32_t j = 0; j < t->wait_more_count; ++j) {
                const CtrObject* m = &p->objects[t->wait_more[j]];
                printf("%s %s %" PRIu32, j ? "," : t->wait_all ? " and all of" : " or", types[m->type < 12 ? m->type : 0],
                       t->wait_more[j]);
                if (m->type == CTR_OBJ_EVENT) printf(" (%s)", event_owner(p, t->wait_more[j]));
            }
            if (w->type == CTR_OBJ_ARBITER) {
                /* the words there: an SDK critical section is its count, its owner's TLS and its depth */
                uint32_t words[3];
                printf(" at 0x%08" PRIx32, t->waiting_address);
                if (snx_ctr_read_ram(p->space, t->waiting_address, words, sizeof words) == SNX_CTR_MEM_OK)
                    printf(" [0x%08" PRIx32 " 0x%08" PRIx32 " 0x%08" PRIx32 "]", words[0], words[1], words[2]);
            }
            if (t->waiting_service[0]) printf(" (the service %s)", t->waiting_service);
        } else if (t->wake_ns)
            printf(", sleeps%s", t->wake_ns == UINT64_MAX ? " forever" : "");
        putchar('\n');
        report_stack(p, cpu);
    }
    printf("  idle: %.1f s in %" PRIu64 " waits for a deadline with no thread to run\n", (double)p->idle_ns / 1e9,
           p->idle_waits);
    /* the SVCs it made most */
    printf("  SVCs:");
    bool shown[256] = {false};
    for (unsigned n = 0; n < 8; ++n) {
        unsigned best = 256;
        for (unsigned i = 0; i < 256; ++i)
            if (!shown[i] && p->svc_counts[i] && (best == 256 || p->svc_counts[i] > p->svc_counts[best])) best = i;
        if (best == 256) break;
        shown[best] = true;
        printf(" 0x%02x x%" PRIu64, best, p->svc_counts[best]);
    }
    putchar('\n');
}
/* The game folder's copy of the runtime, named after it, which plays the game as a Switch game's copy does
   (snx_place_runtime). Nothing when runtime is the copy itself */
static bool deploy_runtime(const char* directory, const char* runtime, char* error, size_t error_size) {
#ifdef _WIN32
    char* copy = snx_ctr_game_file(directory, ".exe");
#else
    char* copy = snx_ctr_game_file(directory, "");
#endif
    int placed = !runtime ? 0 : copy ? snx_place_runtime(runtime, copy, error, error_size) : -1;
    if (placed == 1) printf("The game's runtime is %s\n", copy);
    if (placed < 0 && copy == NULL) snprintf(error, error_size, "out of memory for the game's runtime");
    free(copy);
    return placed >= 0;
}
int snx_ctr_aot_run(const char* directory, const SnxCtrCia* cia, SnxCtrModel model, uint64_t limit, double seconds,
                    unsigned flags, uint32_t ipc_stubs, const char* runtime, char* error, size_t error_size) {
    SnxCtrImage image = {0};
    SnxCtrProcess* process = NULL;
    SnxCtrJit* jit = NULL;
    SnxCtrNative native = {0};
    char *module_path = NULL, *frames = NULL, *wav = NULL;
    int result = 1;
    if (!(cia ? snx_ctr_image_from_cia(cia, &image, error, error_size)
              : snx_ctr_image_load(directory, &image, error, error_size)))
        goto done;
    module_path = snx_ctr_game_file(directory, SNX_CTR_MODULE_SUFFIX);
    {
        /* an older version's hints and module (native/code.so), under the names a Switch game's have: the module is
           checked against the image as any is when it loads */
        char *old = snx_path_join(directory, "hints"), *hints = snx_ctr_game_file(directory, SNX_CTR_HINTS);
        char *native = snx_path_join(directory, "native"), *code = native ? snx_path_join(native, "code.so") : NULL;
        if (old && hints && snx_is_file(old) && !snx_path_exists(hints)) (void)snx_replace(old, hints);
        if (code && module_path && snx_is_file(code) && !snx_path_exists(module_path) &&
            !snx_replace(code, module_path))
            (void)snx_remove_tree(native);
        free(code);
        free(native);
        free(hints);
        free(old);
    }
    /* like the Switch driver's recompile: only when needed, an existing module is no error */
    if ((flags & SNX_CTR_RECOMPILE) || !snx_ctr_native_open(module_path, &image, &native, error, error_size)) {
        snx_ctr_native_close(&native);
        printf("Recompiling ARM11 (offline static AOT)\n");
        int status = snx_ctr_aot_compile(directory, cia, error, error_size);
        if (status) {
            result = status == 3 ? 3 : 1;
            goto done;
        }
        if (!snx_ctr_native_open(module_path, &image, &native, error, error_size)) goto done;
    }
    /* last, as the Switch driver deploys it: a copy beside its module is a game ready to play */
    if (!deploy_runtime(directory, runtime, error, error_size)) goto done;
    if (flags & SNX_CTR_COMPILE_ONLY) {
        result = 0;
        goto done;
    }
    process = snx_ctr_process_new(&image, model);
    if (!process) {
        snprintf(error, error_size, "cannot create checked ARM11 process mappings");
        goto done;
    }
    process->ipc_stubs = ipc_stubs;
    char why[256];
    if (!(jit = snx_ctr_jit_new(process, why, sizeof why)))
        printf("No run-time compiler (%s): only the static blocks run\n", why);
    if (!(cia ? snx_ctr_process_mount_cia(process, directory, cia) : snx_ctr_process_mount_fs(process, directory))) {
        snprintf(error, error_size, cia ? "cannot mount the CIA's filesystem" : "cannot index extracted title filesystem");
        goto done;
    }
    /* Vulkan-only presentation: real frames land in <output>/frames as PPM.
       Without a Vulkan loader/device no frame is shown or fabricated. */
    frames = snx_path_join(directory, "frames");
    if (!snx_is_dir(frames) && plat_mkdir(frames)) {
        char why[256];
        snprintf(error, error_size, "cannot create %s: %s", frames, snx_os_error(why, sizeof why));
        goto done;
    }
    /* the game's name, else its title ID, in the window's title; nothing is added to a 3DS game yet (no update or DLC
       is loaded) */
    char id[17];
    snprintf(id, sizeof id, "%016llX", (unsigned long long)process->title_id);
    printf("Game: %s (%s) - %s\n", process->title[0] ? process->title : id, id, snx_build_info());
    if (!snx_ctr_present_init(process->memory, frames))
        printf("No Vulkan device: no frames will be drawn, shown or saved\n");
    else
        snx_ctr_present_title(process->title[0] ? process->title : id, "");
    /* the DSP's output plays on the default device, and its first two minutes go to <output>/audio.wav */
    wav = snx_path_join(directory, "audio.wav");
    if (!snx_ctr_audio_init(wav)) printf("No audio output: the DSP's frames will not be heard or saved\n");
    snx_ctr_input_init(); /* the scripted presses count from here */
    /* Runs until the game exits or its window is closed, as a Switch game does, or for the --max-blocks budget. The
       dispatch goes in slices so the window's events are seen between them */
    uint64_t executed = 0;
    bool closed = false, timed = false;
    double deadline = seconds > 0 ? snx_seconds() + seconds : 0;
    SnxCtrRunResult state;
    printf("Running ARM11 static AOT%s\n", snx_ctr_space_base(process->space) ? " in a flat 4 GiB space" : "");
    /* a host fault in the flat space is the guest's data abort, reported with the block it happened in */
    ctr_host_faults(snx_ctr_space_base(process->space), &snx_ctr_process_cpu(process)->r[15], track_fault,
                    process->space);
    for (;;) {
        uint64_t slice = SNX_CTR_RUN_SLICE, n = 0;
        if (limit && limit - executed < slice) slice = limit - executed;
        state = snx_ctr_dispatch(native.module, process, slice, &n);
        executed += n;
        if (state != SNX_CTR_RUN_BUDGET || (limit && executed >= limit)) break;
        if ((closed = snx_ctr_present_closed())) break;
        if ((timed = deadline && snx_seconds() >= deadline)) break;
    }
    SnxArmCPU* cpu = snx_ctr_process_cpu(process);
    if (state == SNX_CTR_RUN_EXIT) {
        /* ExitThread ends the process when it was the last thread, and its r0 means nothing then */
        if (process->last_svc == 0x09)
            printf("Guest's last thread exited: blocks=%" PRIu64 "\n", executed);
        else
            printf("Guest ExitProcess: r0=%" PRIu32 " blocks=%" PRIu64 "\n", cpu->r[0], executed);
        result = 0;
    } else if (closed || timed) {
        printf("%s: blocks=%" PRIu64 "\n", closed ? "Closed" : "Ran its time", executed);
        report_threads(process);
        result = 0;
    } else {
        snprintf(error, error_size,
                 "%s: pc=0x%08" PRIx32 " exit=%u syndrome=0x%08" PRIx32 " blocks=%" PRIu64 " svcs=%" PRIu64
                 " last_svc=0x%02" PRIx32 " ipc_service=%" PRIu32 " ipc_header=0x%08" PRIx32 " wait_service=%s",
                 state == SNX_CTR_RUN_BLOCKED       ? "guest blocked (no runnable thread)"
                 : state == SNX_CTR_RUN_BUDGET      ? "static block budget exhausted"
                 : state == SNX_CTR_RUN_UNSUPPORTED ? "unsupported guest operation or missing static entry"
                                                    : "guest fault",
                 cpu->r[15], cpu->exit, cpu->syndrome, executed, process->svc_count, process->last_svc,
                 process->last_ipc_service, process->last_ipc_header, snx_ctr_thread(process)->waiting_service);
        result = state == SNX_CTR_RUN_FAULT ? 1 : 3;
        report_threads(process);
    }
    if (snx_ctr_jit_compiled(jit)) {
        size_t hinted = snx_ctr_jit_save_hints(jit, directory);
        printf("Compiled at run time: %" PRIu64 " blocks", snx_ctr_jit_compiled(jit));
        if (hinted) printf(", %zu new in the title's hints, which a recompile (-r) starts from", hinted);
        putchar('\n');
    }
done:
    ctr_host_faults(NULL, NULL, NULL, NULL);
    snx_ctr_jit_free(jit);
    snx_ctr_present_free();
    snx_ctr_audio_free();
    snx_ctr_input_free();
    snx_ctr_process_free(process);
    free(wav);
    free(frames);
    snx_ctr_native_close(&native);
    free(module_path);
    snx_ctr_image_free(&image);
    return result;
}
