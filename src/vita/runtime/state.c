/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A Vita game's save state, in sections each with the schema hash of what it holds:

     PROCESS  the VitaProcess as it is, but for what the running process keeps (the guest memory's reservation, the
              image, the folders, the subsystems' pointers, the import tables, the run-time compiler) and for the
              files (IO) and the times (TIME): each thread with its registers, waits and fibers, the objects, the
              memory blocks, the display, the dialogs
     LINKS    what of it is a pointer, as indices: the threads running and pacing the frames, each thread's place in
              the module_start chain
     TIME     the times the process keeps by the host's clock (its start, the next vblank, each thread's deadline and
              when it was first seen ready, each sound port's end of play), from the save's now: a load counts them
              from its own
     MEMORY   the image's segments, the runtime's page with the threads' TLS, the imported variables' slots and every
              memory block, in that order
     IO, GXM, AUDIO, NGS, TROPHY, AUDIODEC  the files open, sceGxm, the sound ports, SceNgs, the trophies and the
              audio decoders, each its own (io.c, gxm.c, audio.c, ngs.c, trophy.c, audiodec.c)

   The structs go as their bytes, as the Switch's g_hle does: a state whose types had other fields has another schema,
   and is refused. The game's identity is its title ID's and its image's segments' hash */
#include "vita/runtime/state.h"
#include "vita/gpu/render.h"
#include "vita/hle/hle.h"
#include "vita/runtime/watch.h"
#include "common/platform/time.h"
#include "common/utilities/bits.h"
#include "common/utilities/build_info.h"
#include "common/utilities/common.h"
#include "common/utilities/state_header.h"
#include "common/utilities/state_file.h"
#include "common/utilities/state_io.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PAGE    0x1000u
#define NO_TIME INT64_MIN /* a time the process doesn't keep (0 in it) */
#define REGIONS (VITA_SEGMENTS + 2 + VITA_ALLOCATIONS)

typedef struct {
    uint32_t current, pacer;             /* the threads' indices plus 1, 0 for none */
    uint32_t module_start[VITA_THREADS]; /* each thread's place in the module_start chain (vita_module_start_resume) */
} Links;

/* the game: its title ID and its image's segments */
static uint64_t identity(const VitaProcess* p) {
    uint64_t h = snx_fnv1a(SNX_FNV_BASIS, p->title_id, strlen(p->title_id));
    for (unsigned i = 0; i < p->image.segment_count; ++i) {
        const VitaSegment* s = &p->image.segment[i];
        h = snx_fnv1a(h, &s->address, sizeof s->address);
        h = snx_fnv1a(h, &s->size, sizeof s->size);
        h = snx_fnv1a(h, s->bytes, s->size);
    }
    return h;
}

/* the schema hashes: each struct's walk and size, and the elements of the arrays of structs it holds */
static uint64_t process_schema(const VitaProcess* p) {
    uint64_t h = SNX_FNV_BASIS ^ sizeof *p;
    __builtin_dump_struct(p, snx_state_schema_out, &h);
    __builtin_dump_struct(&p->threads[0], snx_state_schema_out, &h);
    __builtin_dump_struct(&p->threads[0].call.stack[0], snx_state_schema_out, &h);
    __builtin_dump_struct(&p->objects[0], snx_state_schema_out, &h);
    __builtin_dump_struct(&p->files[0], snx_state_schema_out, &h);
    __builtin_dump_struct(&p->allocations[0], snx_state_schema_out, &h);
    __builtin_dump_struct(&p->fibers[0], snx_state_schema_out, &h);
    return h;
}

/* the guest memory a state holds, in its order: [at, at + size) each */
static size_t regions(const VitaProcess* p, const VitaAllocation* allocations, uint32_t* at, uint32_t* size) {
    size_t n = 0;
    for (unsigned i = 0; i < p->image.segment_count; ++i)
        at[n] = p->image.segment[i].address, size[n++] = (p->image.segment[i].size + PAGE - 1) & ~(PAGE - 1);
    at[n] = VITA_RUNTIME_PAGE, size[n++] = VITA_TLS_BASE + VITA_THREADS * VITA_TLS_SIZE - VITA_RUNTIME_PAGE;
    if (p->image.variable_count)
        at[n] = VITA_VARIABLES, size[n++] = (p->image.variable_count * VITA_VARIABLE_SIZE + PAGE - 1) & ~(PAGE - 1);
    for (unsigned i = 0; i < VITA_ALLOCATIONS; ++i)
        if (allocations[i].size) at[n] = allocations[i].address, size[n++] = allocations[i].size;
    return n;
}
static uint64_t memory_bytes(const uint32_t* size, size_t n) {
    uint64_t total = 0;
    for (size_t i = 0; i < n; ++i) total += size[i];
    return total;
}

/* the times the process keeps by the host's clock, in the TIME section's order: their places in p */
static size_t times(VitaProcess* p, uint64_t** out) {
    size_t n = 0;
    out[n++] = &p->start_ns;
    out[n++] = &p->display.next_ns;
    for (unsigned i = 0; i < VITA_THREADS; ++i) {
        out[n++] = &p->threads[i].wait.deadline;
        out[n++] = &p->threads[i].ready_ns;
    }
    for (uint64_t* busy; (busy = vita_audio_state_time(p, (unsigned)(n - 2 - VITA_THREADS * 2))) != NULL;)
        out[n++] = busy;
    return n;
}
#define TIMES_MOST (2 + VITA_THREADS * 2 + 64)

/* what a state can't hold, which the game holds now: 0, or -1 with it in why */
static int savable(VitaProcess* p, char* why, size_t whysize) {
    if (p->current) {
        snprintf(why, whysize, "the thread \"%s\" is running: a state is saved between two threads' slices",
                 p->current->name);
        return -1;
    }
    if (p->exited) {
        snprintf(why, whysize, "the game exited (%" PRId32 ")", p->exit_code);
        return -1;
    }
    for (unsigned i = 0; i < VITA_THREADS; ++i) {
        const VitaThread* t = &p->threads[i];
        if (t->state == VITA_THREAD_FREE || !t->call.active || (vita_in_module_start(t) && !t->call.depth)) continue;
        snprintf(why, whysize,
                 "the thread \"%s\" is inside a call of the runtime's into the game (a sort's comparison, a callback, "
                 "%u nested), which the runtime's own code waits on: a state can't hold it",
                 t->name, t->call.depth);
        return -1;
    }
    struct {
        unsigned n;
        const char* what;
    } held[] = {{vita_mp4_in_use(p), "mp4 files open"},
                {vita_videodec_in_use(p), "video decoders"},
                {vita_net_in_use(p), "sockets open"}};
    for (size_t i = 0; i < sizeof held / sizeof *held; ++i)
        if (held[i].n) {
            snprintf(why, whysize, "the game holds %u %s, which a state can't keep", held[i].n, held[i].what);
            return -1;
        }
    return 0;
}

/* the process as a state holds it: what the running process keeps zeroed */
static void strip(VitaProcess* c) {
    c->mem = NULL;
    memset(&c->image, 0, sizeof c->image);
    c->module = NULL;
    c->folder = c->app = c->patch = c->ux0 = c->savedata = NULL;
    memset(c->files, 0, sizeof c->files);
    c->page_refs = NULL;
    c->current = c->pacer = NULL;
    c->start_ns = 0;
    c->display.next_ns = 0;
    c->gxm = NULL;
    c->audio = NULL;
    c->mp4 = NULL;
    c->videodec = NULL;
    c->audiodec = NULL;
    c->ngs = NULL;
    c->jit = NULL;
    c->net = NULL;
    c->trophy = NULL;
    c->hle = c->hle_export = NULL;
    c->hle_export_known = NULL;
    c->unknown_logged = NULL;
    for (unsigned i = 0; i < VITA_THREADS; ++i) {
        VitaThread* t = &c->threads[i];
        t->cpu.mem = t->own.mem = NULL;
        t->lookup = NULL;
        t->direct = NULL;
        t->call.next = NULL;
        t->call.context = NULL;
        memset(t->call.stack, 0, sizeof t->call.stack);
        t->wait.deadline = 0;
        t->ready_ns = 0;
    }
    for (unsigned i = 0; i < VITA_FIBERS; ++i) c->fibers[i].cpu.mem = NULL;
}

static int writer_put(void* w, const void* p, u64 n) { return sw_put((StateWriter*)w, p, n); }
static int reader_get(void* r, void* p, u64 n) { return sr_get((StateReader*)r, p, n); }

int snx_vita_state_save(VitaProcess* p, const char* path, const uint8_t* thumb, char* why, size_t whysize) {
    uint64_t t0 = plat_ns();
    why[0] = 0;
    if (savable(p, why, whysize)) return -1;
    SnxStateHeader h = {0};
    h.format = SNX_STATE_FORMAT;
    h.abi = SNX_VITA_STATE_VERSION;
    h.title_id = identity(p);
    h.created = (uint64_t)time(NULL);
    h.guest_ns = plat_ns() - p->start_ns;
    h.thumb_w = thumb ? SNX_STATE_THUMB_W : 0;
    h.thumb_h = thumb ? SNX_STATE_THUMB_H : 0;
    snprintf(h.runtime, sizeof h.runtime, "%s", snx_build_info());
    snprintf(h.game, sizeof h.game, "%s", p->title_id);
    char* folder = snx_dirname(path);
    if (folder && folder[0] && snx_mkdirs(folder)) {
        char os[160];
        snprintf(why, whysize, "its folder %s can't be made: %s", folder, snx_os_error(os, sizeof os));
        free(folder);
        return -1;
    }
    free(folder);
    VitaProcess* c = malloc(sizeof *c);
    uint32_t* at = malloc(REGIONS * 2 * sizeof *at);
    uint32_t* size = at ? at + REGIONS : NULL;
    if (!c || !at) {
        snprintf(why, whysize, "out of memory for a copy of the process (%zu bytes)", sizeof *c);
        free(c);
        free(at);
        return -1;
    }
    /* the GPU's work done and its writes in guest memory first: what memory holds is then the frame's */
    StateWriter* w = NULL;
    Links links = {0};
    links.pacer = p->pacer ? (uint32_t)(p->pacer - p->threads) + 1 : 0;
    for (unsigned i = 0; i < VITA_THREADS; ++i)
        if (vita_in_module_start(&p->threads[i]))
            links.module_start[i] = (uint32_t)(uintptr_t)p->threads[i].call.context;
    uint64_t* fields[TIMES_MOST];
    size_t nt = times(p, fields);
    int64_t offsets[TIMES_MOST];
    int r = -1;
    StateIo io = {NULL, writer_put, NULL};
    int failed = 0;
    {
        /* gxm first: a refusal of its (a scene being drawn) comes before the file is made */
        w = sw_open(path, &h, thumb, why, whysize);
        if (!w) goto done;
        io.ctx = w;
        failed = sw_begin(w, "GXM", vita_gxm_state_schema()) || vita_gxm_state_save(p, &io, why, whysize) || sw_end(w);
        if (failed) goto done;
    }
    uint64_t now = plat_ns();
    for (size_t i = 0; i < nt; ++i) offsets[i] = *fields[i] ? (int64_t)(*fields[i] - now) : NO_TIME;
    memcpy(c, p, sizeof *c); /* its padding too, which goes in the file */
    strip(c);
    size_t nr = regions(p, p->allocations, at, size);
    failed = sw_begin(w, "PROCESS", process_schema(p)) || sw_put(w, c, sizeof *c) || sw_end(w) ||
             sw_begin(w, "LINKS", 1) || sw_put(w, &links, sizeof links) || sw_end(w) || sw_begin(w, "TIME", 1) ||
             io_put32(&io, (uint32_t)nt) || sw_put(w, offsets, nt * sizeof *offsets) || sw_end(w) ||
             sw_begin(w, "MEMORY", 1);
    for (size_t i = 0; !failed && i < nr; ++i) failed = sw_put(w, p->mem + at[i], size[i]);
    failed = failed || sw_end(w) || sw_begin(w, "IO", 1) || vita_io_state_save(p, &io) || sw_end(w) ||
             sw_begin(w, "AUDIO", vita_audio_state_schema()) || vita_audio_state_save(p, &io) || sw_end(w) ||
             sw_begin(w, "NGS", vita_ngs_state_schema()) || vita_ngs_state_save(p, &io) || sw_end(w) ||
             sw_begin(w, "TROPHY", vita_trophy_state_schema()) || vita_trophy_state_save(p, &io) || sw_end(w) ||
             sw_begin(w, "AUDIODEC", vita_audiodec_state_schema()) || vita_audiodec_state_save(p, &io) || sw_end(w);
    if (!failed) {
        r = sw_finish(w, why, whysize);
        w = NULL;
    }
done:
    if (w) {
        sw_abort(w);
        if (!why[0]) snprintf(why, whysize, "the file couldn't be written");
    }
    free(c);
    free(at);
    if (!r) printf("Saved the state %s in %.1f ms\n", path, (double)(plat_ns() - t0) / 1e6);
    return r;
}

/* the section tagged tag begun, with the schema this runtime writes (0: any): 0, or -1 with why */
static int begin(StateReader* r, const char* tag, uint64_t schema, char* why, size_t whysize) {
    uint64_t had = 0;
    if (sr_begin(r, tag, &had)) {
        snprintf(why, whysize, "it has no %s section", tag);
        return -1;
    }
    if (had != schema) {
        snprintf(why, whysize, "it holds the Vita's %s in another layout (schema %016llX, this ScarletNX's %016llX)",
                 tag, (unsigned long long)had, (unsigned long long)schema);
        return -1;
    }
    return 0;
}
static int get_whole(StateReader* r, const char* tag, void* p, uint64_t n, char* why, size_t whysize) {
    if (sr_left(r) != n) {
        snprintf(why, whysize, "its %s section holds %llu bytes, not the %llu this ScarletNX's holds", tag,
                 (unsigned long long)sr_left(r), (unsigned long long)n);
        return -1;
    }
    if (sr_get(r, p, n)) {
        snprintf(why, whysize, "its %s section is damaged: its data or its checksum doesn't read back", tag);
        return -1;
    }
    return 0;
}
/* a subsystem's section parsed: what it made, or NULL with why, also when bytes are left over */
static void* parsed(StateReader* r, const char* tag, void* staged, char* why, size_t whysize) {
    if (staged && sr_left(r)) {
        snprintf(why, whysize, "its %s section holds %llu bytes past what this ScarletNX reads of it", tag,
                 (unsigned long long)sr_left(r));
        return NULL;
    }
    return staged;
}
/* the MEMORY section read through, n bytes, its checksums checked, into into when it isn't NULL (regions of guest
   memory then): 0, or -1 */
static int memory_read(StateReader* r, VitaProcess* p, const uint32_t* at, const uint32_t* size, size_t n, bool into) {
    enum { CHUNK = 1u << 20 };
    unsigned char* scratch = into ? NULL : malloc(CHUNK);
    if (sr_begin(r, "MEMORY", NULL) || (!into && !scratch)) {
        free(scratch);
        return -1;
    }
    int failed = 0;
    for (size_t i = 0; !failed && i < n; ++i)
        for (uint32_t done = 0; !failed && done < size[i];) {
            uint32_t k = size[i] - done < CHUNK ? size[i] - done : CHUNK;
            failed = sr_get(r, into ? p->mem + at[i] + done : scratch, k);
            done += k;
        }
    free(scratch);
    return failed ? -1 : 0;
}

int snx_vita_state_load(VitaProcess* p, const char* path, char* why, size_t whysize) {
    uint64_t t0 = plat_ns();
    SnxStateHeader h;
    why[0] = 0;
    if (p->current) {
        snprintf(why, whysize, "the thread \"%s\" is running: a state is loaded between two threads' slices",
                 p->current->name);
        return -1;
    }
    StateReader* r = sr_open(path, &h, why, whysize);
    if (!r) return -1;
    StateIo io = {r, NULL, reader_get};
    VitaProcess* c = malloc(sizeof *c);
    uint32_t* at = malloc(REGIONS * 2 * sizeof *at);
    uint32_t* size = at ? at + REGIONS : NULL;
    Links links;
    int64_t offsets[TIMES_MOST];
    uint32_t nt = 0;
    size_t nr = 0;
    void *files = NULL, *gxm = NULL, *audio = NULL, *ngs = NULL, *trophy = NULL, *audiodec = NULL;
    int refused = 1;
    if (!c || !at)
        snprintf(why, whysize, "out of memory for the state's process (%zu bytes)", sizeof *c);
    else if (h.title_id != identity(p))
        snprintf(why, whysize, "it is another game's: its identity is %016llX, this game's (%s) %016llX",
                 (unsigned long long)h.title_id, p->title_id, (unsigned long long)identity(p));
    else if (h.abi != SNX_VITA_STATE_VERSION)
        snprintf(why, whysize, "it is a Vita state of version %u, and this ScarletNX reads version %u", (unsigned)h.abi,
                 (unsigned)SNX_VITA_STATE_VERSION);
    else if (!begin(r, "PROCESS", process_schema(p), why, whysize) &&
             !get_whole(r, "PROCESS", c, sizeof *c, why, whysize) && !begin(r, "LINKS", 1, why, whysize) &&
             !get_whole(r, "LINKS", &links, sizeof links, why, whysize) && !begin(r, "TIME", 1, why, whysize)) {
        const uint32_t least = 2 + VITA_THREADS * 2;
        if (io_get32(&io, &nt) || nt > TIMES_MOST || sr_left(r) != nt * sizeof *offsets ||
            sr_get(r, offsets, nt * sizeof *offsets))
            snprintf(why, whysize, "its TIME section is damaged or cut short");
        else if (nt < least || links.current > VITA_THREADS || links.pacer > VITA_THREADS)
            snprintf(why, whysize,
                     "its TIME or LINKS section is damaged: %u times (%u at least), threads %u and %u (%u at most)", nt,
                     least, links.current, links.pacer, VITA_THREADS);
        else if ((nr = regions(p, c->allocations, at, size)) == 0 || memory_read(r, p, at, size, nr, false))
            snprintf(why, whysize,
                     "its MEMORY section is damaged or cut short: it doesn't read back as %zu regions of "
                     "%llu bytes",
                     nr, (unsigned long long)memory_bytes(size, nr));
        else if (!begin(r, "IO", 1, why, whysize) &&
                 (files = parsed(r, "IO", vita_io_state_open(p, &io, why, whysize), why, whysize)) &&
                 !begin(r, "GXM", vita_gxm_state_schema(), why, whysize) &&
                 (gxm = parsed(r, "GXM", vita_gxm_state_parse(&io, why, whysize), why, whysize)) &&
                 !begin(r, "AUDIO", vita_audio_state_schema(), why, whysize) &&
                 (audio = parsed(r, "AUDIO", vita_audio_state_parse(&io, why, whysize), why, whysize)) &&
                 !begin(r, "NGS", vita_ngs_state_schema(), why, whysize) &&
                 (ngs = parsed(r, "NGS", vita_ngs_state_parse(&io, why, whysize), why, whysize)) &&
                 !begin(r, "TROPHY", vita_trophy_state_schema(), why, whysize) &&
                 (trophy = parsed(r, "TROPHY", vita_trophy_state_parse(&io, why, whysize), why, whysize)) &&
                 !begin(r, "AUDIODEC", vita_audiodec_state_schema(), why, whysize) &&
                 (audiodec = parsed(r, "AUDIODEC", vita_audiodec_state_parse(&io, why, whysize), why, whysize)))
            refused = 0;
    }
    if (refused) {
        sr_close(r);
        vita_io_state_drop(files);
        vita_gxm_state_drop(gxm);
        free(audio);
        vita_ngs_state_drop(ngs);
        vita_trophy_state_drop(trophy);
        vita_audiodec_state_drop(audiodec);
        free(c);
        free(at);
        if (!why[0]) snprintf(why, whysize, "it couldn't be read");
        return -1;
    }
    /* everything read and checked: past here nothing of the running game is kept, and a failure ends the game. The
       renderer goes first, so nothing it still holds of the running game lands in the state's memory */
    vita_render_free(p);
    vita_mp4_free(p);
    vita_videodec_free(p);
    vita_net_free(p);
    char fatal[256] = "";
    if (vita_memory_adopt(p, c->allocations, fatal, sizeof fatal)) {
        for (size_t i = 0; i < nr; ++i) vita_watch_release(at[i], size[i]);
        if (memory_read(r, p, at, size, nr, true))
            snprintf(fatal, sizeof fatal, "its guest memory didn't read back the second time (the disk?)");
    }
    sr_close(r);
    /* the process: the state's, the running one's own parts kept (the image back by a copy of it in c) */
    memcpy(&c->image, &p->image, sizeof c->image);
    memcpy(c->files, p->files, sizeof c->files); /* IO's install closes them */
    c->mem = p->mem;
    c->module = p->module;
    c->folder = p->folder, c->app = p->app, c->patch = p->patch, c->ux0 = p->ux0, c->savedata = p->savedata;
    c->page_refs = p->page_refs;
    c->gxm = p->gxm, c->audio = p->audio, c->mp4 = p->mp4, c->videodec = p->videodec;
    c->audiodec = p->audiodec, c->ngs = p->ngs, c->jit = p->jit, c->net = p->net, c->trophy = p->trophy;
    c->hle = p->hle, c->hle_export = p->hle_export, c->hle_export_known = p->hle_export_known;
    c->unknown_logged = p->unknown_logged;
    memcpy(p, c, sizeof *p);
    for (unsigned i = 0; i < VITA_THREADS; ++i) {
        VitaThread* t = &p->threads[i];
        t->cpu.mem = t->own.mem = p->mem;
        if (links.module_start[i]) vita_module_start_resume(t, links.module_start[i]);
    }
    for (unsigned i = 0; i < VITA_FIBERS; ++i) p->fibers[i].cpu.mem = p->mem;
    p->current = links.current ? &p->threads[links.current - 1] : NULL;
    p->pacer = links.pacer ? &p->threads[links.pacer - 1] : NULL;
    vita_io_state_install(p, files);
    vita_gxm_state_install(p, gxm);
    vita_audio_state_install(p, audio);
    vita_ngs_state_install(p, ngs);
    vita_trophy_state_install(p, trophy);
    vita_audiodec_state_install(p, audiodec);
    uint64_t* fields[TIMES_MOST];
    size_t have = times(p, fields);
    int64_t now = (int64_t)plat_ns();
    for (size_t i = 0; i < have && i < nt; ++i) *fields[i] = offsets[i] == NO_TIME ? 0 : (uint64_t)(now + offsets[i]);
    free(c);
    free(at);
    if (fatal[0]) {
        fprintf(stderr, "vita: the save state %s was loaded only in part, and the game can't go on: %s\n", path, fatal);
        p->exited = true;
        p->exit_code = -1;
        return 0;
    }
    printf("Loaded the state %s in %.1f ms\n", path, (double)(plat_ns() - t0) / 1e6);
    return 0;
}
