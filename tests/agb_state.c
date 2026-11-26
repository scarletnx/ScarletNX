/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The GBA's save states (src/agb/runtime/state.c). A ROM the test makes keeps every device busy: two timers, one
   feeding direct sound through DMA1 and one interrupting, the square channel retuned each frame, the vertical blank's
   interrupt through the BIOS's wait, a line drawn on the bitmap screen each frame and copied by DMA3, the cartridge's
   prefetch unit on. The frame's count and a random number run on in registers, the IRQ mode's stack moves with the
   frame, and a busy stretch of a frame or more with interrupts held off, loading words of the ROM's start, has
   vertical blanks land inside the ROM's code with the prefetch unit in any state; the frame shows a timer's count of
   the cycles where the stretch ends, so the frames show the machine's time to the cycle. A state saved after 33
   frames, loaded in the same machine and in a fresh one that ran on its own meanwhile, gives the same next 30 frames as
   the run that went on: each frame's picture and sound word for word, and the same state after them, section by
   section. A state of another game, of another version, with a section of another layout, damaged or cut short is
   refused before anything changes, its reason naming the values; its header and thumbnail read as the launcher reads
   them */
#include "agb/board/board.h"
#include "agb/cpu/agb_cpu.h"
#include "agb/cpu/memmap.h"
#include "agb/cpu/recomp.h"
#include "agb/runtime/state.h"
#include "common/platform/time.h"
#include "common/utilities/common.h"
#include "common/utilities/endian.h"
#include "common/utilities/state_header.h"
#include "common/utilities/hotkeys.h"
#include "common/utilities/vpad_consoles.h"
#include "common/utilities/vpad_sdl.h"
#include "common/utilities/state_file.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expression)                                                                                              \
    do {                                                                                                               \
        if (!(expression)) {                                                                                           \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression);                                           \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (0)

/* the frames before the state: it lands in the busy stretch, the prefetch unit reading on, where the fresh machine's
   8th frame ends in the BIOS's wait, the unit stopped */
#define WARM         33
#define FRESH        8
#define FRAMES       30 /* the frames after it, compared */
#define FRAME_CYCLES (AGB_SCANLINE_CYCLES * AGB_FRAME_LINES)
#define PIXELS       (AGB_SCREEN_WIDTH * AGB_SCREEN_HEIGHT)

/* The ROM's code at 0x080000C0, after its header (llvm-mc -triple=armv4t) */
static const uint32_t code[] = {
    0xE3A0C301, /* start: mov r12, #0x04000000 */
    0xE59F01DC, /* ldr r0, =(handler - start + 0x080000C0) */
    0xE59F11DC, /* ldr r1, =0x03007FFC */
    0xE5810000, /* str r0, [r1] */
    0xE59F01D8, /* ldr r0, =0x4317 */
    0xE28C1C02, /* add r1, r12, #0x200 */
    0xE1C100B4, /* strh r0, [r1, #4] */
    0xE3A00402, /* mov r0, #0x02000000 */
    0xE3A01000, /* mov r1, #0 */
    0xE59F21C8, /* ldr r2, =0x9E3779B1 */
    0xE0030291, /* fill: mul r3, r1, r2 */
    0xE7803101, /* str r3, [r0, r1, lsl #2] */
    0xE2811001, /* add r1, r1, #1 */
    0xE3510901, /* cmp r1, #0x4000 */
    0x1AFFFFFA, /* bne fill */
    0xE59F01B4, /* ldr r0, =0x0403 */
    0xE1CC00B0, /* strh r0, [r12] */
    0xE3A00080, /* mov r0, #0x80 */
    0xE1CC08B4, /* strh r0, [r12, #0x84] */
    0xE59F01A8, /* ldr r0, =0xFF77 */
    0xE1CC08B0, /* strh r0, [r12, #0x80] */
    0xE59F01A4, /* ldr r0, =0x0B06 */
    0xE1CC08B2, /* strh r0, [r12, #0x82] */
    0xE3A00000, /* mov r0, #0 */
    0xE1CC06B0, /* strh r0, [r12, #0x60] */
    0xE59F0198, /* ldr r0, =0xF080 */
    0xE1CC06B2, /* strh r0, [r12, #0x62] */
    0xE3A00402, /* mov r0, #0x02000000 */
    0xE58C00BC, /* str r0, [r12, #0xBC] */
    0xE59F018C, /* ldr r0, =0x040000A0 */
    0xE58C00C0, /* str r0, [r12, #0xC0] */
    0xE59F0188, /* ldr r0, =0xB640 */
    0xE1CC0CB6, /* strh r0, [r12, #0xC6] */
    0xE59F0184, /* ldr r0, =0x0080FC00 */
    0xE58C0100, /* str r0, [r12, #0x100] */
    0xE59F0180, /* ldr r0, =0x00C18000 */
    0xE58C0108, /* str r0, [r12, #0x108] */
    0xE3A00502, /* mov r0, #0x00800000 */
    0xE58C010C, /* str r0, [r12, #0x10C] */
    0xE3A00008, /* mov r0, #8 */
    0xE1CC00B4, /* strh r0, [r12, #4] */
    0xE28C1C02, /* add r1, r12, #0x200 */
    0xE3A00021, /* mov r0, #0x21 */
    0xE1C100B0, /* strh r0, [r1] */
    0xE3A00001, /* mov r0, #1 */
    0xE1C100B8, /* strh r0, [r1, #8] */
    0xE3A06000, /* mov r6, #0 */
    0xE59F7154, /* ldr r7, =0x12345678 */
    0xE59F9154, /* ldr r9, =0x41C64E6D */
    0xE59FA154, /* ldr r10, =12345 */
    0xEF050000, /* frame: swi 0x050000 */
    0xE2866001, /* add r6, r6, #1 */
    0xE321F0D2, /* msr cpsr_c, #0xD2 */
    0xE59FD148, /* ldr sp, =0x03007FA0 */
    0xE3160001, /* tst r6, #1 */
    0x124DD040, /* subne sp, sp, #0x40 */
    0xE321F01F, /* msr cpsr_c, #0x1F */
    0xE1A00006, /* mov r0, r6 */
    0xE3A010A0, /* mov r1, #160 */
    0xE1500001, /* row: cmp r0, r1 */
    0x20400001, /* subhs r0, r0, r1 */
    0x2AFFFFFC, /* bhs row */
    0xE3A01E1E, /* mov r1, #480 */
    0xE0050190, /* mul r5, r0, r1 */
    0xE2855406, /* add r5, r5, #0x06000000 */
    0xE59F111C, /* ldr r1, =0x123 */
    0xE0020196, /* mul r2, r6, r1 */
    0xE59F1118, /* ldr r1, =0x7FFF */
    0xE0022001, /* and r2, r2, r1 */
    0xE3A03000, /* mov r3, #0 */
    0xE0851083, /* line: add r1, r5, r3, lsl #1 */
    0xE1C120B0, /* strh r2, [r1] */
    0xE2822001, /* add r2, r2, #1 */
    0xE2833001, /* add r3, r3, #1 */
    0xE35300F0, /* cmp r3, #240 */
    0x1AFFFFF9, /* bne line */
    0xE58C50D4, /* str r5, [r12, #0xD4] */
    0xE2850C96, /* add r0, r5, #0x9600 */
    0xE59F10F0, /* ldr r1, =0x06012C00 */
    0xE1500001, /* cmp r0, r1 */
    0x20400001, /* subhs r0, r0, r1 */
    0x22800406, /* addhs r0, r0, #0x06000000 */
    0xE58C00D8, /* str r0, [r12, #0xD8] */
    0xE59F00E0, /* ldr r0, =0x800000F0 */
    0xE58C00DC, /* str r0, [r12, #0xDC] */
    0xE3A01025, /* mov r1, #37 */
    0xE0000196, /* mul r0, r6, r1 */
    0xE59F10D4, /* ldr r1, =0x7FF */
    0xE0000001, /* and r0, r0, r1 */
    0xE3800902, /* orr r0, r0, #0x8000 */
    0xE1CC06B4, /* strh r0, [r12, #0x64] */
    0xE2068007, /* and r8, r6, #7 */
    0xE1A08588, /* mov r8, r8, lsl #11 */
    0xE3A03302, /* mov r3, #0x08000000 */
    0xE321F09F, /* msr cpsr_c, #0x9F */
    0xE02BA997, /* busy: mla r11, r7, r9, r10 */
    0xE20B00FC, /* and r0, r11, #0xFC */
    0xE7930000, /* ldr r0, [r3, r0] */
    0xE02B7000, /* eor r7, r11, r0 */
    0xE2588001, /* subs r8, r8, #1 */
    0x5AFFFFF9, /* bpl busy */
    0xE321F01F, /* msr cpsr_c, #0x1F */
    0xE59F108C, /* ldr r1, =0x7FFF */
    0xE0070001, /* and r0, r7, r1 */
    0xE3A01406, /* mov r1, #0x06000000 */
    0xE1C100B0, /* strh r0, [r1] */
    0xE28C2C01, /* add r2, r12, #0x100 */
    0xE1D200BC, /* ldrh r0, [r2, #0x0C] */
    0xE1C100B2, /* strh r0, [r1, #2] */
    0xEAFFFFC3, /* b frame */
    0xE2801C02, /* handler: add r1, r0, #0x200 */
    0xE1D120B2, /* ldrh r2, [r1, #2] */
    0xE1C120B2, /* strh r2, [r1, #2] */
    0xE59F3070, /* ldr r3, =0x03007FF8 */
    0xE1D300B0, /* ldrh r0, [r3] */
    0xE1800002, /* orr r0, r0, r2 */
    0xE1C300B0, /* strh r0, [r3] */
    0xE3A03403, /* mov r3, #0x03000000 */
    0xE7930102, /* ldr r0, [r3, r2, lsl #2] */
    0xE2800001, /* add r0, r0, #1 */
    0xE7830102, /* str r0, [r3, r2, lsl #2] */
    0xE12FFF1E, /* bx lr */
    0x08000278, /* the interrupt handler */
    0x03007FFC, /* its vector */
    0x00004317, /* WAITCNT: prefetch on */
    0x9E3779B1, /* noise */
    0x00000403, /* DISPCNT: mode 3, BG2 */
    0x0000FF77, /* SOUNDCNT_L: PSG at full, both sides */
    0x00000B06, /* SOUNDCNT_H: FIFO A at full, both sides, reset */
    0x0000F080, /* SOUND1CNT_H: volume 15, duty 50% */
    0x040000A0, /* FIFO A */
    0x0000B640, /* DMA1CNT_H: special start, repeat, 32 bit, fixed destination */
    0x0080FC00, /* TM0: 16384 Hz */
    0x00C18000, /* TM2: interrupt, prescaler 64 */
    0x12345678, /* the random number's start */
    0x41C64E6D, /* its multiplier */
    0x00003039, /* its increment */
    0x03007FA0, /* the IRQ stack */
    0x00000123, /* colour step */
    0x00007FFF, /* colour mask */
    0x06012C00, /* the frame's end in VRAM */
    0x800000F0, /* DMA3CNT: 240 halfwords, now */
    0x000007FF, /* tone mask */
    0x03007FF8, /* IntrCheck */
};

static void put32(uint8_t* p, uint32_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}
/* the ROM, titled title: another title is another game */
static uint8_t* make_rom(const char* title, size_t* size) {
    *size = 0xC0 + sizeof code;
    uint8_t* rom = (uint8_t*)calloc(1, *size);
    if (!rom) return NULL;
    put32(rom, 0xEA00002E); /* b 0x080000C0, the header's entry branch */
    memcpy(rom + 0xA0, title, strlen(title) < 12 ? strlen(title) : 12);
    rom[0xB2] = 0x96;
    unsigned sum = 0;
    for (int i = 0xA0; i <= 0xBC; ++i) sum += rom[i];
    rom[0xBD] = (uint8_t)(0u - sum - 0x19u);
    for (size_t i = 0; i < sizeof code / sizeof *code; ++i) put32(rom + 0xC0 + i * 4, code[i]);
    return rom;
}

typedef struct {
    AgbBoard board;
    AgbMap map;
    AgbCpu cpu;
    AgbRecomp recomp;
    int16_t* audio;
} Machine;
/* the ROM in a machine of its own, every block compiled as the run reaches it: 0, or 1 having said why */
static int machine_open(Machine* m, const char* title, const char* base) {
    char error[512] = {0};
    size_t size = 0;
    uint8_t* rom = make_rom(title, &size);
    m->audio = (int16_t*)calloc(2 * AGB_APU_RATE, sizeof(int16_t));
    CHECK(rom && m->audio);
    agb_board_init(&m->board);
    if (agb_board_insert(&m->board, rom, size, error, sizeof error) ||
        agb_map_init(&m->map, &m->board, error, sizeof error)) {
        fprintf(stderr, "agb_state: %s\n", error);
        free(rom);
        return 1;
    }
    free(rom);
    agb_cpu_init(&m->cpu, &m->board, m->map.base);
    agb_recomp_init(&m->recomp, base);
    return 0;
}
static void machine_close(Machine* m) {
    agb_recomp_free(&m->recomp);
    agb_map_destroy(&m->map, &m->board);
    agb_board_destroy(&m->board);
    free(m->audio);
}
/* What the frames gave: each one's picture and the count of its stereo samples, and the samples */
typedef struct {
    uint32_t* picture;
    uint32_t written[FRAMES];
    int16_t* sound;
    size_t samples;
} Output;
/* frames of the machine, kept in out when it isn't NULL: 0, or 1 having said why */
static int run(Machine* m, int frames, Output* out) {
    char error[512] = {0};
    for (int f = 0; f < frames; ++f) {
        uint32_t written = 0;
        if (agb_cpu_run(&m->board, &m->cpu, &m->recomp, FRAME_CYCLES, m->audio, AGB_APU_RATE, &written, error,
                        sizeof error)) {
            fprintf(stderr, "agb_state: %s\n", error);
            return 1;
        }
        if (!out) continue;
        memcpy(out->picture + (size_t)f * PIXELS, m->board.ppu.framebuffer, sizeof m->board.ppu.framebuffer);
        out->written[f] = written;
        memcpy(out->sound + out->samples * 2, m->audio, written * 2 * sizeof *m->audio);
        out->samples += written;
    }
    return 0;
}
/* the pictures, then the sound, in one allocation */
static int output_new(Output* o) {
    *o = (Output){0};
    o->picture = (uint32_t*)malloc((size_t)FRAMES * PIXELS * sizeof *o->picture +
                                   (size_t)FRAMES * 2 * AGB_APU_RATE * sizeof *o->sound);
    o->sound = o->picture ? (int16_t*)(o->picture + (size_t)FRAMES * PIXELS) : NULL;
    return o->picture ? 0 : -1;
}
static void output_free(Output* o) { free(o->picture); }
/* the two runs' frames word for word: 0, or 1 naming the first that differs */
static int same_output(const char* what, const Output* a, const Output* b) {
    for (int f = 0; f < FRAMES; ++f) {
        const uint32_t *pa = a->picture + (size_t)f * PIXELS, *pb = b->picture + (size_t)f * PIXELS;
        for (size_t i = 0; i < PIXELS; ++i)
            if (pa[i] != pb[i]) {
                fprintf(stderr, "agb_state: %s: frame %d's pixel %zu is %08X, the run that went on had %08X\n", what, f,
                        i, pb[i], pa[i]);
                return 1;
            }
        if (a->written[f] != b->written[f]) {
            fprintf(stderr, "agb_state: %s: frame %d gave %u samples, the run that went on %u\n", what, f,
                    b->written[f], a->written[f]);
            return 1;
        }
    }
    for (size_t i = 0; i < a->samples * 2; ++i)
        if (a->sound[i] != b->sound[i]) {
            fprintf(stderr, "agb_state: %s: sample %zu is %d, the run that went on had %d\n", what, i, b->sound[i],
                    a->sound[i]);
            return 1;
        }
    return 0;
}
/* every section of two states, byte for byte: 0, or 1 naming the first that differs and where */
static int same_state(const char* what, const char* a, const char* b) {
    static const char* const tags[] = {"BOARD", "CPU", "MEMORY"};
    SnxStateHeader ha, hb;
    char why[256] = {0};
    StateReader *ra = sr_open(a, &ha, why, sizeof why), *rb = ra ? sr_open(b, &hb, why, sizeof why) : NULL;
    int r = !ra || !rb;
    if (r) fprintf(stderr, "agb_state: %s: %s\n", what, why);
    for (size_t t = 0; !r && t < sizeof tags / sizeof *tags; ++t) {
        uint64_t sa = 0, sb = 0;
        if (sr_begin(ra, tags[t], &sa) || sr_begin(rb, tags[t], &sb) || sa != sb || sr_left(ra) != sr_left(rb)) {
            fprintf(stderr, "agb_state: %s: the %s sections differ in their schema or size\n", what, tags[t]);
            r = 1;
            break;
        }
        uint64_t n = sr_left(ra);
        uint8_t *pa = (uint8_t*)malloc(n), *pb = (uint8_t*)malloc(n);
        if (!pa || !pb || sr_get(ra, pa, n) || sr_get(rb, pb, n)) r = 1;
        for (uint64_t i = 0; !r && i < n; ++i)
            if (pa[i] != pb[i]) {
                fprintf(stderr, "agb_state: %s: the %s sections differ first at byte %llu (%02X, not %02X)\n", what,
                        tags[t], (unsigned long long)i, pb[i], pa[i]);
                r = 1;
            }
        free(pa);
        free(pb);
    }
    sr_close(ra);
    sr_close(rb);
    return r;
}
/* a copy of the file at from, its byte at offset (from the end where it is negative) xored with flip, or the file cut
   to its first cut bytes when cut isn't 0 */
static int copy_changed(const char* from, const char* to, long long offset, uint8_t flip, size_t cut) {
    size_t size = 0;
    uint8_t* bytes = (uint8_t*)snx_read_file(from, &size);
    if (!bytes) return -1;
    if (cut) size = cut;
    if (flip) bytes[offset < 0 ? (long long)size + offset : offset] ^= flip;
    int r = snx_write_file(to, bytes, size);
    free(bytes);
    return r;
}
/* a load of path refused, its reason holding each of the texts, and the machine as it was: 0, or 1 having said why */
static int refused(Machine* m, const char* dir, const char* path, const char* const* texts) {
    char why[512] = {0}, *before = snx_format("%s/agb_state_before.snxstate", dir),
         *after = snx_format("%s/agb_state_after.snxstate", dir);
    CHECK(before && after);
    CHECK(!snx_agb_state_save(&m->board, &m->cpu, before, NULL, why, sizeof why));
    if (!snx_agb_state_load(&m->board, &m->cpu, path, why, sizeof why)) {
        fprintf(stderr, "agb_state: %s loaded\n", path);
        return 1;
    }
    printf("refused %s: %s\n", path, why);
    for (; *texts; ++texts)
        if (!strstr(why, *texts)) {
            fprintf(stderr, "agb_state: the refusal of %s says \"%s\", without \"%s\"\n", path, why, *texts);
            return 1;
        }
    CHECK(!snx_agb_state_save(&m->board, &m->cpu, after, NULL, why, sizeof why));
    if (same_state("a refused load", before, after)) return 1;
    remove(before);
    remove(after);
    free(before);
    free(after);
    return 0;
}

/* the window's last line about the save states (vpad_sdl.c's snx_vpad_window_states) */
static char g_said[320];
static void said(const char* line) { snprintf(g_said, sizeof g_said, "%s", line); }
/* the GBA's pair as the runner's loop hands it to the window */
static int pair_save(void* game, const char* path, const uint8_t* thumb, char* why, size_t whysize) {
    Machine* m = (Machine*)game;
    return snx_agb_state_save(&m->board, &m->cpu, path, thumb, why, whysize);
}
static int pair_load(void* game, const char* path, char* why, size_t whysize) {
    Machine* m = (Machine*)game;
    return snx_agb_state_load(&m->board, &m->cpu, path, why, whysize);
}
/* The window's keys on them, as a Switch game's F7, F8 and F9: the slot F9 picks, a save to it and over it, an empty
   one said so, a load with the game first in the auto slot; each said as a Switch game's */
static int keys(Machine* m, const char* dir) {
    char* prefix = snx_format("%s/", dir);
    SnxVpadStates states = {prefix, "a game", m, pair_save, pair_load};
    char *two = snx_format("%s/states/a game.2.snxstate", dir),
         *back = snx_format("%s/states/a game.auto.snxstate", dir);
    CHECK(prefix && two && back);
    remove(two), remove(back); /* what a run that stopped before its end left */
    snx_vpad_window_open(NULL, NULL, said, NULL);
    snx_vpad_window_states(HOTKEY_NEXT_SLOT, &states, NULL);
    CHECK(!strcmp(g_said, "Slot 2: empty"));
    snx_vpad_window_states(HOTKEY_SAVE_STATE, &states, NULL);
    CHECK(!strcmp(g_said, "Saved to slot 2") && snx_is_file(two));
    snx_vpad_window_states(HOTKEY_SAVE_STATE, &states, NULL);
    CHECK(!strcmp(g_said, "Saved over slot 2"));
    snx_vpad_window_states(HOTKEY_NEXT_SLOT, &states, NULL);
    snx_vpad_window_states(HOTKEY_LOAD_STATE, &states, NULL);
    CHECK(!strcmp(g_said, "Slot 3 is empty") && !snx_is_file(back));
    for (int i = 0; i < 9; ++i) snx_vpad_window_states(HOTKEY_NEXT_SLOT, &states, NULL); /* 10 slots: round to 2 */
    CHECK(!strncmp(g_said, "Slot 2: 20", 10));
    snx_vpad_window_states(HOTKEY_LOAD_STATE, &states, NULL);
    CHECK(!strcmp(g_said, "Loaded slot 2") && snx_is_file(back));
    { /* an unattended run's saves: each due once at its time, a save's picture asked for only then, none left after */
        char* args[] = {"scarletnx-agb", "--state-save=100:1,200:auto", NULL};
        char* one = snx_format("%s/states/a game.1.snxstate", dir);
        vpad_console(&snx_vpad_gba); /* the runner's: its settings are a console's */
        snx_vpad_sdl_settings();
        vpad_settings(dir, "a game", 2, args);
        CHECK(one && snx_vpad_window_states_waiting() && !snx_vpad_window_save_due(50) &&
              snx_vpad_window_save_due(100));
        snx_vpad_window_states_due(150, &states, NULL);
        CHECK(snx_is_file(one) && !snx_vpad_window_save_due(150) && snx_vpad_window_states_waiting());
        snx_vpad_window_states_due(250, &states, NULL);
        CHECK(!strncmp(g_said, "Saved ", 6) && !snx_vpad_window_states_waiting() && !snx_vpad_window_save_due(1000));
        remove(one);
        free(one);
    }
    snx_vpad_window_close();
    remove(two), remove(back);
    free(two), free(back), free(prefix);
    return 0;
}
int main(int argc, char** argv) {
    const char* dir = argc > 1 ? argv[1] : ".";
    char why[512] = {0}, *base = snx_format("%s/agb_state.code", dir),
         *saved = snx_format("%s/agb_state_a.snxstate", dir), *went_on = snx_format("%s/agb_state_b.snxstate", dir),
         *again = snx_format("%s/agb_state_c.snxstate", dir), *fresh = snx_format("%s/agb_state_d.snxstate", dir),
         *changed = snx_format("%s/agb_state_x.snxstate", dir);
    CHECK(base && saved && went_on && again && fresh && changed);
    Machine m, n;
    Output ref, loaded, other;
    CHECK(!output_new(&ref) && !output_new(&loaded) && !output_new(&other));
    if (machine_open(&m, "STATE TEST", base) || run(&m, WARM, NULL)) return 1;
    /* the thumbnail a runner gives, a gradient the header must give back */
    uint8_t* thumb = (uint8_t*)malloc((size_t)SNX_STATE_THUMB_W * SNX_STATE_THUMB_H * 4);
    CHECK(thumb);
    for (size_t i = 0; i < (size_t)SNX_STATE_THUMB_W * SNX_STATE_THUMB_H * 4; ++i) thumb[i] = (uint8_t)(i * 7);
    uint64_t t0 = plat_ns();
    CHECK(!snx_agb_state_save(&m.board, &m.cpu, saved, thumb, why, sizeof why));
    uint64_t t1 = plat_ns();
    if (run(&m, FRAMES, &ref)) return 1;
    CHECK(!snx_agb_state_save(&m.board, &m.cpu, went_on, NULL, why, sizeof why));
    /* the ROM drew and played: most frames differ from the one before (a busy stretch past a frame draws none), and
       the sound isn't silence */
    {
        int moved = 0;
        for (int f = 1; f < FRAMES; ++f)
            moved += memcmp(ref.picture + (size_t)(f - 1) * PIXELS, ref.picture + (size_t)f * PIXELS,
                            PIXELS * sizeof *ref.picture) != 0;
        CHECK(moved >= FRAMES / 2);
    }
    {
        size_t loud = 0;
        for (size_t i = 0; i < ref.samples * 2; ++i) loud += ref.sound[i] != ref.sound[0];
        CHECK(ref.samples > 0 && loud > ref.samples);
    }
    /* loaded in the machine that went on */
    uint64_t t2 = plat_ns();
    if (snx_agb_state_load(&m.board, &m.cpu, saved, why, sizeof why)) {
        fprintf(stderr, "agb_state: %s\n", why);
        return 1;
    }
    uint64_t t3 = plat_ns();
    if (run(&m, FRAMES, &loaded) || same_output("loaded in the same machine", &ref, &loaded)) return 1;
    CHECK(!snx_agb_state_save(&m.board, &m.cpu, again, NULL, why, sizeof why));
    if (same_state("loaded in the same machine", went_on, again)) return 1;
    /* loaded in a fresh machine that ran frames of its own: nothing of it stays */
    if (machine_open(&n, "STATE TEST", base) || run(&n, FRESH, NULL)) return 1;
    if (snx_agb_state_load(&n.board, &n.cpu, saved, why, sizeof why)) {
        fprintf(stderr, "agb_state: %s\n", why);
        return 1;
    }
    if (run(&n, FRAMES, &other) || same_output("loaded in a fresh machine", &ref, &other)) return 1;
    CHECK(!snx_agb_state_save(&n.board, &n.cpu, fresh, NULL, why, sizeof why));
    if (same_state("loaded in a fresh machine", went_on, fresh)) return 1;
    /* the header and the thumbnail, as the launcher reads them */
    {
        SnxStateHeader h;
        uint8_t* back = NULL;
        CHECK(!snx_state_peek(saved, &h, &back, why, sizeof why));
        CHECK(h.abi == SNX_AGB_STATE_VERSION && h.title_id == agb_board_hash(&m.board) &&
              !strcmp(h.game, "STATE TEST"));
        CHECK(h.guest_ns > (uint64_t)WARM * 16000000 && h.guest_ns < (uint64_t)(WARM + 1) * 17000000);
        CHECK(h.thumb_w == SNX_STATE_THUMB_W && h.thumb_h == SNX_STATE_THUMB_H && back &&
              !memcmp(back, thumb, (size_t)SNX_STATE_THUMB_W * SNX_STATE_THUMB_H * 4));
        free(back);
    }
    {
        size_t size = 0;
        void* bytes = snx_read_file(saved, &size);
        CHECK(bytes);
        free(bytes);
        printf("a GBA state: %zu bytes with its %dx%d thumbnail, saved in %.1f ms, loaded in %.1f ms\n", size,
               SNX_STATE_THUMB_W, SNX_STATE_THUMB_H, (double)(t1 - t0) / 1e6, (double)(t3 - t2) / 1e6);
    }
    /* refused: another game's, another version's, a section of another layout, a damaged one, one cut short */
    {
        Machine o;
        char hash[24], mine[24];
        if (machine_open(&o, "OTHER GAME", base) || run(&o, 3, NULL)) return 1;
        snprintf(hash, sizeof hash, "%016llX", (unsigned long long)agb_board_hash(&m.board));
        snprintf(mine, sizeof mine, "%016llX", (unsigned long long)agb_board_hash(&o.board));
        const char* const game[] = {"another game's", hash, mine, NULL};
        if (refused(&o, dir, saved, game)) return 1;
        machine_close(&o);
    }
    {
        size_t size = 0;
        uint8_t* bytes = (uint8_t*)snx_read_file(saved, &size);
        CHECK(bytes && size > SNX_STATE_HEADER);
        uint64_t table = snx_ld_le64(bytes + 48); /* state_header.c: the section table's offset */
        free(bytes);
        const char* const version[] = {"version 9", "version 1", NULL};
        CHECK(!copy_changed(saved, changed, 12, 1 ^ 9, 0)); /* the header's abi, 1 made 9 */
        if (refused(&n, dir, changed, version)) return 1;
        const char* const layout[] = {"BOARD", "another layout", NULL};
        CHECK(!copy_changed(saved, changed, (long long)table + 8, 0x5A, 0)); /* the first entry's schema */
        if (refused(&n, dir, changed, layout)) return 1;
        const char* const damaged[] = {"MEMORY", "damaged", NULL};
        CHECK(!copy_changed(saved, changed, (long long)table - 6, 0x40, 0)); /* the last section's last frame */
        if (refused(&n, dir, changed, damaged)) return 1;
        const char* const cut[] = {"not written to its end", NULL};
        CHECK(!copy_changed(saved, changed, 0, 0, (size_t)table + 20));
        if (refused(&n, dir, changed, cut)) return 1;
    }
    if (keys(&m, dir)) return 1;
    machine_close(&n);
    machine_close(&m);
    output_free(&ref);
    output_free(&loaded);
    output_free(&other);
    free(thumb);
    remove(saved), remove(went_on), remove(again), remove(fresh), remove(changed);
    free(base), free(saved), free(went_on), free(again), free(fresh), free(changed);
    return 0;
}
