/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The 3DS's save states (src/3ds/runtime/state.c), on a process of the test's driven through its SVCs and IPC as a
   game drives them: a thread asleep and one running, with their registers set, an event, a semaphore and a timer, heap
   and linear memory, a file and a folder open on the SD card, the DSP playing a PCM16 buffer from FCRAM, a PICA with
   a command list, the AOT blocks a CRO made stale. It is saved, and what follows the save is recorded: the DSP's next
   frames, the file's bytes, the folder's next entry, the objects' answers, the registers, the memory and the tick count.
   More work then undoes it all (memory written and unmapped, the file and folder closed, the DSP gone, the registers
   and objects changed, a second gone by); a load gives the same record word for word, and so does a fresh process's. A
   state saved right after a load holds what the first held. Another version, another game, a damaged file and a file
   gone since are refused with nothing changed, and immediate-mode vertices not drawn refuse a save */
#include "3ds/runtime/state.h"
#include "3ds/runtime/internal.h"
#include "3ds/audio/internal.h"
#include "3ds/board/clock.h"
#include "3ds/gpu/pica.h"
#include "3ds/ipc/ctr_ipc.h"
#include "3ds/services/dsp.h"
#include "common/utilities/common.h"
#include "common/utilities/state_file.h"
#include "snx_settings.h"
#include "support/3ds_guest.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expression)                                                                                              \
    do {                                                                                                               \
        if (!(expression)) {                                                                                           \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression);                                           \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)
#define R0      (SNX_CTR_DSP_RAM + DSP_REGION0)
#define R1      (SNX_CTR_DSP_RAM + DSP_REGION1)
#define SAMPLES 2400u /* the buffer the DSP plays: 15 frames */
#define BUFFER  0x301000u
#define PATHS   0x300400u
#define TEXT    0x100000u

static uint32_t svc(SnxCtrProcess* p, uint32_t number, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t r4,
                    SnxCtrSvcResult expected) {
    uint32_t* r = p->cpu.state.cpu.r;
    r[0] = r0, r[1] = r1, r[2] = r2, r[3] = r3, r[4] = r4;
    CHECK(snx_ctr_svc(p, number) == expected);
    return r[1];
}
static uint32_t memory(SnxCtrProcess* p, uint32_t op, uint32_t va, uint32_t size) {
    uint32_t got = svc(p, 1, op, va, 0, size, 3, SNX_CTR_SVC_RETURN);
    CHECK(!p->cpu.state.cpu.r[0]);
    return got;
}
static void path16(SnxCtrProcess* p, const char* path) {
    unsigned char b[64] = {0};
    for (size_t i = 0; path[i]; ++i) b[i * 2] = (unsigned char)path[i];
    CHECK(snx_ctr_write_ram(p->space, PATHS, b, sizeof b) == SNX_CTR_MEM_OK);
}
static int16_t sample(unsigned k) { return (int16_t)((int)(k * 37 % 2000) - 1000); }
static void phys(SnxCtrProcess* p, uint32_t pa, const void* data, size_t size) {
    CHECK(snx_ctr_phys_write(p->memory, pa, data, size) == SNX_CTR_MEM_OK);
}

/* what a game's process holds: the handles it uses */
typedef struct {
    uint32_t fs, sdmc_lo, sdmc_hi, file, dir, event, semaphore, timer, sleeper, heap, linear, linear_pa;
} Game;

/* the DSP on, a mono PCM16 buffer in linear memory playing on mix 0 and on mix 1 through a delay, a second buffer
   queued after it */
static void dsp_play(SnxCtrProcess* p, Game* g) {
    CHECK(!snx_ctr_dsp_open(p) && p->dsp.core);
    const uint8_t init[4] = {0};
    CHECK(!snx_ctr_dsp_pipe_write(p->dsp.core, SNX_CTR_DSP_PIPE_AUDIO, init, sizeof init));
    int16_t pcm[SAMPLES];
    for (unsigned k = 0; k < SAMPLES; ++k) pcm[k] = sample(k);
    phys(p, g->linear_pa, pcm, sizeof pcm);
    for (unsigned k = 0; k < SAMPLES; ++k) pcm[k] = sample(k + 7);
    phys(p, g->linear_pa + sizeof pcm, pcm, sizeof pcm / 3);
    uint16_t counter[2] = {2, 1};
    phys(p, R0 + DSP_FRAME_COUNTER, &counter[0], 2);
    phys(p, R1 + DSP_FRAME_COUNTER, &counter[1], 2);
    DspSourceConfig c = {0};
    c.dirty = DSP_ENABLE_DIRTY | DSP_INTERPOLATION_DIRTY | DSP_RATE_MULTIPLIER_DIRTY | DSP_GAIN_0_DIRTY |
              DSP_GAIN_0_DIRTY << 1 | DSP_FORMAT_DIRTY | DSP_MONO_OR_STEREO_DIRTY | DSP_EMBEDDED_BUFFER_DIRTY |
              DSP_BUFFER_QUEUE_DIRTY;
    c.enable = 1;
    c.interpolation_mode = DSP_LINEAR;
    c.rate_multiplier = 0.75f;
    c.gain[0][0] = c.gain[0][1] = 0.5f;
    c.gain[1][0] = c.gain[1][1] = 0.25f;
    c.buffers_dirty = 1;
    c.buffers[0] = (DspBuffer){
        .physical_address = dsp32(g->linear_pa + SAMPLES * 2), .length = dsp32(SAMPLES / 3), .buffer_id = 4};
    c.flags1 = (uint16_t)(DSP_MONO | DSP_PCM16 << 2);
    c.physical_address = dsp32(g->linear_pa);
    c.length = dsp32(SAMPLES);
    c.buffer_id = 3;
    phys(p, R0 + DSP_SOURCE_CONFIGS, &c, sizeof c);
    DspConfig master = {.dirty = DSP_MASTER_VOLUME_DIRTY | DSP_AUX_RETURN_VOLUME_0_DIRTY | DSP_DELAY_EFFECT_0_DIRTY,
                        .master_volume = 1.0f,
                        .aux_return_volume = {1.0f},
                        .delay = {{.dirty = 1, .enable = 1, .frame_count = 2, .g = 16, .a = 64, .b = 32}}};
    phys(p, R0 + DSP_CONFIGURATION, &master, sizeof master);
    int16_t frame[SNX_CTR_DSP_SAMPLES][2];
    for (unsigned i = 0; i < 2; ++i) CHECK(snx_ctr_dsp_tick(p->dsp.core, frame));
}

/* the game's work before its save */
static void play(SnxCtrProcess* p, Game* g) {
    CHECK(!snx_ctr_session_create(p, SNX_CTR_SERVICE_FS, &g->fs));
    uint32_t init[] = {0x8010002, 0x20, 0xffffffff};
    ctr_call(p, g->fs, init, 3, SNX_CTR_SVC_RETURN, 0);
    uint32_t archive[] = {0x80c00c2, 9, 1, 1, 0x4002, 0x300020};
    ctr_call(p, g->fs, archive, 6, SNX_CTR_SVC_RETURN, 0);
    CHECK(!ctr_command_word(p, 1));
    g->sdmc_lo = ctr_command_word(p, 2), g->sdmc_hi = ctr_command_word(p, 3);
    /* a folder of three files, the first written, open as a file and the folder read one entry in */
    path16(p, "d");
    uint32_t mkdir[] = {0x8090182, 0, g->sdmc_lo, g->sdmc_hi, 4, 4, 0, 0x10002, PATHS};
    ctr_call(p, g->fs, mkdir, 9, SNX_CTR_SVC_RETURN, 0);
    const char* names[3] = {"d/a.bin", "d/b.bin", "d/c.bin"};
    for (unsigned i = 0; i < 3; ++i) {
        path16(p, names[i]);
        uint32_t mkfile[] = {0x8080202, 0, g->sdmc_lo, g->sdmc_hi, 4, 16, 0, 4096, 0, 0x40002, PATHS};
        ctr_call(p, g->fs, mkfile, 11, SNX_CTR_SVC_RETURN, 0);
        CHECK(!ctr_command_word(p, 1));
    }
    path16(p, "d/a.bin");
    uint32_t open[] = {0x80201c2, 0, g->sdmc_lo, g->sdmc_hi, 4, 16, 3, 0, 0x40002, PATHS};
    ctr_call(p, g->fs, open, 10, SNX_CTR_SVC_RETURN, 0);
    CHECK(!ctr_command_word(p, 1));
    g->file = ctr_command_word(p, 3);
    unsigned char bytes[512];
    for (unsigned i = 0; i < sizeof bytes; ++i) bytes[i] = (unsigned char)(i * 13 + 5);
    CHECK(snx_ctr_write_ram(p->space, BUFFER, bytes, sizeof bytes) == SNX_CTR_MEM_OK);
    uint32_t write[] = {0x8030102, 0, 0, sizeof bytes, 0, ((uint32_t)sizeof bytes << 4) | 10, BUFFER};
    ctr_call(p, g->file, write, 7, SNX_CTR_SVC_RETURN, 0);
    CHECK(!ctr_command_word(p, 1) && ctr_command_word(p, 2) == sizeof bytes);
    path16(p, "d");
    uint32_t opend[] = {0x80b0102, g->sdmc_lo, g->sdmc_hi, 4, 4, 0x10002, PATHS};
    ctr_call(p, g->fs, opend, 7, SNX_CTR_SVC_RETURN, 0);
    CHECK(!ctr_command_word(p, 1));
    g->dir = ctr_command_word(p, 3);
    uint32_t read[] = {0x8010042, 1, (0x228u << 4) | 12, BUFFER};
    ctr_call(p, g->dir, read, 4, SNX_CTR_SVC_RETURN, 0);
    CHECK(!ctr_command_word(p, 1) && ctr_command_word(p, 2) == 1);
    /* memory: heap pages and linear ones, written */
    g->heap = memory(p, 3, CTR_HEAP, 3 * 4096);
    g->linear = memory(p, 0x10003, 0, 4 * 4096); /* the samples first */
    CHECK(snx_ctr_query(p->space, g->linear, &g->linear_pa, NULL) == SNX_CTR_MEM_OK);
    for (uint32_t i = 0; i < 3 * 4096; i += 4) ctr_put(p, g->heap + i, i * 2654435761u);
    /* objects: an event signalled, a semaphore with two, a timer ten seconds off */
    g->event = svc(p, 0x17, 0, CTR_RESET_STICKY, 0, 0, 0, SNX_CTR_SVC_RETURN);
    svc(p, 0x18, g->event, 0, 0, 0, 0, SNX_CTR_SVC_RETURN);
    g->semaphore = svc(p, 0x15, 0, 2, 5, 0, 0, SNX_CTR_SVC_RETURN);
    CHECK(!p->cpu.state.cpu.r[0]);
    g->timer = svc(p, 0x1a, 0, CTR_RESET_ONESHOT, 0, 0, 0, SNX_CTR_SVC_RETURN);
    svc(p, 0x1b, g->timer, 0, 1410065408u, 2, 0, SNX_CTR_SVC_RETURN); /* 10 s */
    /* a second thread, its registers set, asleep for 5 s: the main thread waits for it to start, which wakes it */
    uint32_t main = p->current_thread, started = svc(p, 0x17, 0, CTR_RESET_ONESHOT, 0, 0, 0, SNX_CTR_SVC_RETURN);
    uint32_t other = svc(p, 8, 0x30, TEXT + 4, 0x1234, SNX_CTR_STACK_TOP - 0x1000, 0xfffffffe, SNX_CTR_SVC_RETURN);
    CHECK(!p->cpu.state.cpu.r[0] && other);
    svc(p, 0x24, started, 0, UINT32_MAX, UINT32_MAX, 0, SNX_CTR_SVC_BLOCKED);
    CHECK(snx_ctr_schedule(p) == SNX_CTR_SCHED_RUN && p->current_thread != main);
    g->sleeper = p->current_thread;
    svc(p, 0x18, started, 0, 0, 0, 0, SNX_CTR_SVC_RETURN);
    SnxArmCPU* cpu = &p->cpu.state.cpu;
    for (unsigned i = 5; i < 13; ++i) cpu->r[i] = 0x01010101u * i;
    svc(p, 0x0a, 0x2a05f200u, 1, 0, 0, 0, SNX_CTR_SVC_BLOCKED);
    CHECK(snx_ctr_schedule(p) == SNX_CTR_SCHED_RUN && p->current_thread == main);
    for (unsigned i = 0; i < 13; ++i) cpu->r[i] = 0x11111111u * i + 7;
    for (unsigned i = 0; i < 32; ++i) cpu->d[i] = 0x0123456789abcdefull * (i + 1);
    cpu->fpscr = 0x03c00010u;
    cpu->tpidrurw = 0xfeedf00du;
    /* the DSP playing; the AOT blocks of one window stale; a PICA with its last list */
    dsp_play(p, g);
    CHECK(snx_ctr_aot_mark_stale(p, TEXT, 4));
    p->gsp.pica = malloc(sizeof *p->gsp.pica);
    CHECK(p->gsp.pica);
    snx_ctr_pica_init(p->gsp.pica);
    p->gsp.pica->regs[0x100] = 0xabcd;
    p->gsp.pica->list = malloc(16);
    CHECK(p->gsp.pica->list);
    for (unsigned i = 0; i < 4; ++i) p->gsp.pica->list[i] = 0x1000u + i;
    p->gsp.pica->list_words = 4, p->gsp.pica->list_index = 2;
}

/* what follows a save, recorded: the registers of both threads, the memory, the DSP's next frames, the file's bytes,
   the folder's next entry, the objects' answers, the object count */
typedef struct {
    unsigned char* bytes;
    size_t size, capacity;
} Record;
static void put(Record* r, const void* data, size_t size) {
    if (r->size + size > r->capacity) {
        r->capacity = (r->size + size) * 2;
        r->bytes = realloc(r->bytes, r->capacity);
        CHECK(r->bytes);
    }
    memcpy(r->bytes + r->size, data, size);
    r->size += size;
}
static void put32(Record* r, uint32_t v) { put(r, &v, sizeof v); }
static Record follow(SnxCtrProcess* p, const Game* g) {
    Record r = {0};
    const SnxArmCPU* cpu = &p->cpu.state.cpu;
    put(&r, cpu->r, sizeof cpu->r);
    put(&r, cpu->d, sizeof cpu->d);
    put32(&r, cpu->cpsr), put32(&r, cpu->fpscr), put32(&r, cpu->tpidrurw), put32(&r, cpu->tpidruro);
    put32(&r, p->current_thread);
    const SnxArmCPU* sleeper = &p->objects[g->sleeper].data.thread.context;
    put(&r, sleeper->r, sizeof sleeper->r);
    for (uint32_t i = 0; i < 3 * 4096; i += 4) put32(&r, ctr_word(p, g->heap + i));
    for (uint32_t i = 0; i < 8192; i += 4) put32(&r, ctr_word(p, g->linear + i));
    int16_t frame[SNX_CTR_DSP_SAMPLES][2];
    CHECK(p->dsp.core);
    for (unsigned i = 0; i < 3; ++i) {
        CHECK(snx_ctr_dsp_tick(p->dsp.core, frame));
        put(&r, frame, sizeof frame);
    }
    unsigned char bytes[64];
    uint32_t read[] = {0x80200c2, 100, 0, sizeof bytes, ((uint32_t)sizeof bytes << 4) | 12, BUFFER};
    ctr_call(p, g->file, read, 6, SNX_CTR_SVC_RETURN, 0);
    CHECK(!ctr_command_word(p, 1) && ctr_command_word(p, 2) == sizeof bytes);
    CHECK(snx_ctr_read_ram(p->space, BUFFER, bytes, sizeof bytes) == SNX_CTR_MEM_OK);
    put(&r, bytes, sizeof bytes);
    unsigned char entry[0x228];
    uint32_t next[] = {0x8010042, 1, (0x228u << 4) | 12, BUFFER};
    ctr_call(p, g->dir, next, 4, SNX_CTR_SVC_RETURN, 0);
    put32(&r, ctr_command_word(p, 1)), put32(&r, ctr_command_word(p, 2));
    CHECK(snx_ctr_read_ram(p->space, BUFFER, entry, sizeof entry) == SNX_CTR_MEM_OK);
    put(&r, entry, sizeof entry);
    /* the semaphore had two: two waits take them, a third times out; the event stays signalled */
    for (unsigned i = 0; i < 3; ++i) {
        svc(p, 0x24, g->semaphore, 0, 0, 0, 0, SNX_CTR_SVC_RETURN);
        put32(&r, p->cpu.state.cpu.r[0]);
    }
    svc(p, 0x24, g->event, 0, 0, 0, 0, SNX_CTR_SVC_RETURN);
    put32(&r, p->cpu.state.cpu.r[0]);
    put32(&r, ctr_object_count(p));
    return r;
}
/* the sleeping thread's wake and the timer's signal, host monotonic ns */
static void deadlines(SnxCtrProcess* p, const Game* g, uint64_t* wake, uint64_t* timer) {
    *wake = p->objects[g->sleeper].data.thread.wake_ns;
    *timer = p->objects[snx_ctr_handle_object(p, g->timer)].data.timer.deadline;
}
/* GetSystemTick, the thread's registers kept */
static uint64_t ticks(SnxCtrProcess* p) {
    uint32_t kept[5];
    memcpy(kept, p->cpu.state.cpu.r, sizeof kept);
    svc(p, 0x28, 0, 0, 0, 0, 0, SNX_CTR_SVC_RETURN);
    uint64_t t = (uint64_t)p->cpu.state.cpu.r[1] << 32 | p->cpu.state.cpu.r[0];
    memcpy(p->cpu.state.cpu.r, kept, sizeof kept);
    return t;
}
static void spin(double seconds) {
    double end = snx_seconds() + seconds;
    while (snx_seconds() < end) {}
}

/* what the game does after its save: everything a load must undo */
static void undo(SnxCtrProcess* p, const Game* g) {
    for (uint32_t i = 0; i < 3 * 4096; i += 4) ctr_put(p, g->heap + i, 0xdeadbeef);
    memory(p, 1, g->heap + 4096, 4096);
    memory(p, 3, CTR_HEAP + 0x100000, 4096);
    uint32_t close[] = {0x8080000}, dclose[] = {0x8020000};
    ctr_call(p, g->file, close, 1, SNX_CTR_SVC_RETURN, 0);
    ctr_call(p, g->dir, dclose, 1, SNX_CTR_SVC_RETURN, 0);
    svc(p, 0x24, g->semaphore, 0, 0, 0, 0, SNX_CTR_SVC_RETURN);
    svc(p, 0x19, g->event, 0, 0, 0, 0, SNX_CTR_SVC_RETURN);
    svc(p, 0x17, 0, CTR_RESET_ONESHOT, 0, 0, 0, SNX_CTR_SVC_RETURN);
    int16_t frame[SNX_CTR_DSP_SAMPLES][2];
    CHECK(snx_ctr_dsp_tick(p->dsp.core, frame));
    snx_ctr_dsp_close(p);
    memset(p->cpu.state.cpu.r, 0x5a, 13 * 4);
    memset(p->cpu.state.cpu.d, 0xa5, sizeof p->cpu.state.cpu.d);
    CHECK(snx_ctr_aot_mark_stale(p, TEXT + 0x1000, 4));
    p->gsp.pica->regs[0x100] = 0x5555;
    p->gsp.pica->list[1] = 0;
    spin(1.0);
}

/* a section of a state file, its bytes */
static unsigned char* section(const char* path, const char* tag, uint64_t* size) {
    SnxStateHeader h;
    char why[256];
    StateReader* r = sr_open(path, &h, why, sizeof why);
    CHECK(r && !sr_begin(r, tag, NULL));
    *size = sr_left(r);
    unsigned char* bytes = malloc(*size ? *size : 1);
    CHECK(bytes && !sr_get(r, bytes, *size));
    sr_close(r);
    return bytes;
}
static void same_sections(const char* a, const char* b) {
    const char* tags[] = {"PROCESS", "MAPS", "MEMORY", "FILES", "LDR", "PICA", "DSP"};
    for (size_t i = 0; i < sizeof tags / sizeof *tags; ++i) {
        uint64_t na, nb;
        unsigned char *x = section(a, tags[i], &na), *y = section(b, tags[i], &nb);
        if (na != nb || memcmp(x, y, na)) {
            fprintf(stderr, "the state saved after a load holds another %s (%" PRIu64 " and %" PRIu64 " bytes)\n",
                    tags[i], na, nb);
            exit(1);
        }
        free(x);
        free(y);
    }
}
static void same(const Record* a, const Record* b, const char* what) {
    if (a->size == b->size && !memcmp(a->bytes, b->bytes, a->size)) return;
    size_t at = 0;
    while (at < a->size && at < b->size && a->bytes[at] == b->bytes[at]) ++at;
    fprintf(stderr, "%s: what follows the load differs from what followed the save at byte %zu of %zu\n", what, at,
            a->size);
    exit(1);
}
/* The host's clock around the save and the load: a deadline was so far from the save's now, somewhere in
   [save_from, save_to], and is as far from the load's, in [load_from, load_to] */
typedef struct {
    uint64_t save_from, save_to, load_from, load_to;
} Moments;
static void moved(uint64_t got, uint64_t saved, const Moments* m, const char* what) {
    uint64_t least = saved - m->save_to + m->load_from - 1000, most = saved - m->save_from + m->load_to + 1000;
    if (got < least || got > most) {
        fprintf(stderr,
                "%s: %" PRIu64 " ns after the load, not in [%" PRIu64 ", %" PRIu64 "] (%" PRIu64 " at the save)\n",
                what, got, least, most, saved);
        exit(1);
    }
}
static long file_size(const char* path) {
    FILE* f = fopen(path, "rb");
    CHECK(f && !fseek(f, 0, SEEK_END));
    long n = ftell(f);
    fclose(f);
    return n;
}
static void copy_file(const char* from, const char* to, long keep, unsigned abi) {
    FILE *in = fopen(from, "rb"), *out = fopen(to, "wb");
    CHECK(in && out);
    unsigned char buffer[65536];
    long done = 0;
    for (size_t n; done < keep && (n = fread(buffer, 1, sizeof buffer, in)) > 0;) {
        if (!done && abi) buffer[12] = (unsigned char)abi;
        size_t k = keep - done < (long)n ? (size_t)(keep - done) : n;
        CHECK(fwrite(buffer, 1, k, out) == k);
        done += (long)k;
    }
    fclose(in);
    CHECK(!fclose(out));
}
static void refused(SnxCtrProcess* p, const Game* g, const char* path, const char* expect) {
    char why[512];
    uint32_t word = ctr_word(p, g->heap), objects = ctr_object_count(p), current = p->current_thread;
    if (!snx_ctr_state_load(p, path, why, sizeof why) || !strstr(why, expect)) {
        fprintf(stderr, "a load of %s: \"%s\", not a refusal naming \"%s\"\n", path, why, expect);
        exit(1);
    }
    CHECK(ctr_word(p, g->heap) == word && ctr_object_count(p) == objects && p->current_thread == current);
    printf("refused: %s\n", why);
}

int main(void) {
    g_settings.ctr_answer_unknown_commands = 0;
    char *root = snx_temp_dir(NULL), why[512] = "";
    CHECK(root);
    char *folder = snx_path_join(root, "game"), *saved = snx_path_join(root, "s.snxstate"),
         *again = snx_path_join(root, "again.snxstate"), *bad = snx_path_join(root, "bad.snxstate");
    CHECK(folder && saved && again && bad && !snx_mkdirs(folder));
    unsigned char code[0x3000] = {0};
    SnxCtrImage image = ctr_image(code, sizeof code, 0x3000);
    SnxCtrProcess* p = snx_ctr_process_new(&image, SNX_CTR_OLD);
    CHECK(p && snx_ctr_process_mount_fs(p, folder));
    Game g = {0};
    play(p, &g);
    /* immediate-mode vertices not drawn refuse a save, naming them */
    p->gsp.pica->immediate_given = true;
    CHECK(snx_ctr_state_save(p, saved, NULL, why, sizeof why) && strstr(why, "immediate-mode"));
    printf("refused: %s\n", why);
    p->gsp.pica->immediate_given = false;
    Moments m;
    uint64_t wake, timer, ticked = snx_ctr_now_ns(), t0 = ticks(p);
    deadlines(p, &g, &wake, &timer);
    m.save_from = snx_ctr_now_ns();
    CHECK(!snx_ctr_state_save(p, saved, NULL, why, sizeof why));
    m.save_to = snx_ctr_now_ns();
    printf("state: %ld bytes\n", file_size(saved));
    Record before = follow(p, &g);
    undo(p, &g);
    m.load_from = snx_ctr_now_ns();
    if (snx_ctr_state_load(p, saved, why, sizeof why)) {
        fprintf(stderr, "3ds_state: %s\n", why);
        return 1;
    }
    m.load_to = snx_ctr_now_ns();
    CHECK(!snx_ctr_state_save(p, again, NULL, why, sizeof why));
    same_sections(saved, again);
    /* the deadlines and the tick count go on from the save's, the second the game went on for in between not counted */
    uint64_t wake_after, timer_after;
    deadlines(p, &g, &wake_after, &timer_after);
    moved(wake_after, wake, &m, "the sleeping thread's wake");
    moved(timer_after, timer, &m, "the timer's signal");
    uint64_t t1 = ticks(p), most = (m.save_to - ticked + snx_ctr_now_ns() - m.load_from) * 268111856.0 / 1e9 + 1000;
    if (t1 < t0 || t1 - t0 > most) {
        fprintf(stderr,
                "the tick count went from %" PRIu64 " at the save to %" PRIu64 " after the load, %" PRIu64 " at most\n",
                t0, t1, t0 + most);
        return 1;
    }
    Record after = follow(p, &g);
    same(&before, &after, "the same process");
    /* a fresh process of the game's, its own state made by the load */
    SnxCtrProcess* q = snx_ctr_process_new(&image, SNX_CTR_OLD);
    CHECK(q && snx_ctr_process_mount_fs(q, folder) && !q->dsp.core);
    if (snx_ctr_state_load(q, saved, why, sizeof why)) {
        fprintf(stderr, "3ds_state: a fresh process: %s\n", why);
        return 1;
    }
    Record fresh = follow(q, &g);
    same(&before, &fresh, "a fresh process");
    snx_ctr_process_free(q);
    /* refusals, with nothing changed */
    copy_file(saved, bad, file_size(saved), 9);
    refused(p, &g, bad, "version 9");
    copy_file(saved, bad, file_size(saved) / 2, 0);
    refused(p, &g, bad, "");
    uint64_t title = p->title_id;
    p->title_id ^= 1;
    refused(p, &g, saved, "another game's");
    p->title_id = title;
    uint32_t close[] = {0x8080000};
    ctr_call(p, g.file, close, 1, SNX_CTR_SVC_RETURN, 0);
    path16(p, "d/a.bin");
    uint32_t remove[] = {0x8040142, 0, g.sdmc_lo, g.sdmc_hi, 4, 16, 0x40002, PATHS};
    ctr_call(p, g.fs, remove, 8, SNX_CTR_SVC_RETURN, 0);
    CHECK(!ctr_command_word(p, 1));
    refused(p, &g, saved, "d/a.bin");
    snx_ctr_process_free(p);
    free(before.bytes);
    free(after.bytes);
    free(fresh.bytes);
    CHECK(!snx_remove_tree(root));
    free(saved), free(again), free(bad), free(folder), free(root);
    puts("3ds_state: ok");
    return 0;
}
