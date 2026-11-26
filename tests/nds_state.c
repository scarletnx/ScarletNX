/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The DS's save states (src/nds/runtime/state.c). The test's ARM9 program runs an interrupt handler and the vertical
   blank's wait, its IRQ mode's stack moving with the frame, and each frame the test writes what a game's frame would:
   a cell of engine A's background and palette colors, one of them the count of a timer started again every eighth
   frame; a triangle for the 3D engine whose color and corner move; a display capture into bank B, which the GPU gives
   back after the frame; a write and a read of the power chip's registers through the ARM7, its answers kept; and every
   fourth frame a command list for the ARM7's sound driver over the FIFO that retunes a square wave, a noise channel
   playing beside it. A state saved after 30 frames, loaded in the same machine and in a fresh one that ran on its own
   meanwhile, gives the same next 30 frames as the run that went on: each frame's picture as the Vulkan device composed
   it, its sound and its VRAM word for word, and the same state after them, section by section. A state of another
   game, of another version, with a section of another layout, damaged or cut short is refused before anything
   changes, its reason naming the values. Exits 77 where the host has no Vulkan 1.3 device */
#include "nds/audio/spu.h"
#include "nds/board/nds.h"
#include "nds/cpu/nds_cpu.h"
#include "nds/cpu/recomp.h"
#include "nds/gpu/vk/vk.h"
#include "nds/runtime/state.h"
#include "common/platform/time.h"
#include "common/utilities/bits.h"
#include "common/utilities/common.h"
#include "common/utilities/endian.h"
#include "common/utilities/state_header.h"
#include "common/utilities/state_file.h"
#include "support/nds_image.h"
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

#define IO      0x04000000u
#define WARM    30                                         /* the frames before the state */
#define FRAMES  30                                         /* the frames after it, compared */
#define PICTURE (NDS_SCREEN_WIDTH * NDS_SCREEN_HEIGHT * 2) /* both screens' pixels, RGBA8 words */
#define SAMPLES 4096u                                      /* a frame's stereo samples at most, and more */
#define LISTS   0x02200000u                                /* where the test's sound command lists go */
#define REPLIES 0x02110000u /* where the test keeps the ARM7's answers it read, 256 words around */

/* The ARM9's program at 0x02000000 (llvm-mc -triple=armv5te): its interrupt handler and the vertical blank's wait
   as the CPU tests' program has them, the frame counted in a register, and the IRQ mode's stack, where the BIOS's
   interrupt entry saves registers, moved with the frame's parity: the handler keeps the stack each interrupt came in
   on */
static const uint32_t program[] = {
    0xE59F009C, /* entry: ldr r0, =0x03003FFC */
    0xE59F109C, /* ldr r1, =(handler - entry + 0x02000000) */
    0xE5801000, /* str r1, [r0] */
    0xE3A00301, /* mov r0, #0x04000000 */
    0xE3A01008, /* mov r1, #8 */
    0xE1C010B4, /* strh r1, [r0, #4] */
    0xE2802C02, /* add r2, r0, #0x200 */
    0xE3A01001, /* mov r1, #1 */
    0xE5821010, /* str r1, [r2, #0x10] */
    0xE5821008, /* str r1, [r2, #8] */
    0xE10F3000, /* mrs r3, cpsr */
    0xE3C33080, /* bic r3, r3, #0x80 */
    0xE121F003, /* msr cpsr_c, r3 */
    0xE3A06000, /* mov r6, #0 */
    0xE3A04621, /* ldr r4, =0x02100000 */
    0xEF050000, /* frame: swi 0x050000 */
    0xE2866001, /* add r6, r6, #1 */
    0xE5846014, /* str r6, [r4, #20] */
    0xE10F3000, /* mrs r3, cpsr */
    0xE321F0D2, /* msr cpsr_c, #0xD2 */
    0xE59FD054, /* ldr sp, =0x03003F80 */
    0xE3160001, /* tst r6, #1 */
    0x124DD040, /* subne sp, sp, #0x40 */
    0xE121F003, /* msr cpsr_c, r3 */
    0xEAFFFFF5, /* b frame */
    0xE3A00301, /* handler: mov r0, #0x04000000 */
    0xE2800C02, /* add r0, r0, #0x200 */
    0xE5901014, /* ldr r1, [r0, #0x14] */
    0xE5801014, /* str r1, [r0, #0x14] */
    0xE59F2034, /* ldr r2, =0x03003FF8 */
    0xE5923000, /* ldr r3, [r2] */
    0xE1833001, /* orr r3, r3, r1 */
    0xE5823000, /* str r3, [r2] */
    0xE59F2028, /* ldr r2, =0x02100018 */
    0xE5923000, /* ldr r3, [r2] */
    0xE2833001, /* add r3, r3, #1 */
    0xE5823000, /* str r3, [r2] */
    0xE203303F, /* and r3, r3, #63 */
    0xE59F2018, /* ldr r2, =0x02100100 */
    0xE782D103, /* str sp, [r2, r3, lsl #2] */
    0xE12FFF1E, /* bx lr */
    0x03003FFC, /* the DTCM's last word, the handler's */
    0x02000064, /* the interrupt handler */
    0x03003F80, /* the IRQ stack */
    0x03003FF8, /* the BIOS's flags */
    0x02100018, /* the interrupts counted */
    0x02100100, /* the IRQ stacks seen */
};
#define PROGRAM_WORDS (sizeof program / sizeof *program)

typedef struct {
    NdsRom rom;
    NdsBoard* board;
    NdsCpu* cpu;
    NdsRecomp* recomp;
} Machine;

static void gx(NdsBoard* board, uint32_t command, uint32_t parameter) {
    nds_io_write(board, IO + 0x400 + (command << 2 & 0x1FC), 4, parameter);
}
/* the image at path in a machine of its own, its blocks compiled as the run reaches them, the display set up as a
   game's would be: 0, or 1 having said why */
static int machine_open(Machine* m, const char* path) {
    char error[512] = "";
    *m = (Machine){0};
    if (!nds_rom_open(&m->rom, path, NULL, error, sizeof error)) {
        fprintf(stderr, "nds_state: %s\n", error);
        return 1;
    }
    m->board = calloc(1, sizeof *m->board);
    m->cpu = calloc(1, sizeof *m->cpu);
    CHECK(m->board && m->cpu);
    if (!nds_board_init(m->board, &m->rom, error, sizeof error)) {
        fprintf(stderr, "nds_state: %s\n", error);
        return 1;
    }
    m->recomp = nds_recomp_new(&m->rom, NULL, error, sizeof error);
    CHECK(m->recomp);
    nds_cpu_init(m->cpu, m->board, m->recomp);
    NdsBoard* b = m->board;
    unsigned char* w = b->mem.base;
    /* engine A: BG1 a text background of 16 colors whose tiles are colors 1 and 2, bank A its memory; BG0 the 3D
       picture in front */
    nds_io_write(b, IO + 0x240, 1, 0x81);
    snx_st_le16(w + 0x05000004, 0x03E0);
    memset(w + 0x06004000, 0x11, 32);
    memset(w + 0x06004020, 0x22, 32);
    nds_io_write(b, IO + 0x00A, 2, 1 << 2 | 1);
    nds_io_write(b, IO + 0x008, 2, 0);
    nds_io_write(b, IO + 0x060, 2, 0);
    nds_io_write(b, IO + 0x350, 4, 0);
    gx(b, 0x10, 0), gx(b, 0x15, 0);
    gx(b, 0x10, 2), gx(b, 0x15, 0);
    gx(b, 0x60, 0 | 0 << 8 | 255u << 16 | 191u << 24);
    nds_io_write(b, IO + 0x000, 4, 0x00010308);
    nds_io_write(b, IO + 0x184, 2, 0x8000);     /* the FIFO on, for the sound driver's commands */
    nds_io_write(b, IO + 0x241, 1, 0x80);       /* bank B for the display capture */
    nds_io_write(b, IO + 0x100, 4, 0x00830000); /* timer 0 counting at its slowest, which the frames show */
    return 0;
}
static void machine_close(Machine* m) {
    if (m->recomp) nds_recomp_free(m->recomp);
    if (m->board) nds_board_free(m->board);
    free(m->board);
    free(m->cpu);
    nds_rom_close(&m->rom);
}
/* the sound driver's command at list: cmd with its four arguments, then next */
static void command(unsigned char* w, uint32_t list, uint32_t next, uint32_t cmd, uint32_t a0, uint32_t a1, uint32_t a2,
                    uint32_t a3) {
    snx_st_le32(w + list, next);
    snx_st_le32(w + list + 4, cmd);
    snx_st_le32(w + list + 8, a0);
    snx_st_le32(w + list + 12, a1);
    snx_st_le32(w + list + 16, a2);
    snx_st_le32(w + list + 20, a3);
}
/* what a game's frame writes, by the frame's number */
static void stimulus(NdsBoard* b, unsigned f) {
    unsigned char* w = b->mem.base;
    snx_st_le16(w + 0x06000000 + (4 * 32 + f % 32) * 2, (uint16_t)(f & 1));
    snx_st_le16(w + 0x05000002, (uint16_t)(f * 0x421u & 0x7FFF));
    snx_st_le16(w + 0x05000004, (uint16_t)(nds_io_read(b, IO + 0x100, 2) & 0x7FFF));
    if (f % 8 == 3) { /* the timer started again from a count of the frame's */
        nds_io_write(b, IO + 0x102, 2, 0);
        nds_io_write(b, IO + 0x100, 2, f * 0x111u);
        nds_io_write(b, IO + 0x102, 2, 0x83);
    }
    gx(b, 0x29, 0x001F00C0);
    gx(b, 0x40, 0);
    gx(b, 0x20, f * 37u & 0x7FFF);
    gx(b, 0x25, (uint16_t)-0x800 | (uint32_t)(uint16_t)-0x800 << 16);
    gx(b, 0x25, 0x800 | (uint32_t)(uint16_t)-0x800 << 16);
    gx(b, 0x25, (uint16_t)(f * 64u - 0x400) | 0x800u << 16);
    gx(b, 0x41, 0);
    gx(b, 0x50, 0);
    /* the frame captured into bank B, which the GPU gives back after the frame: a state between two holds it */
    nds_io_write(b, IO + 0x064, 4, 0x80000000u | 3u << 20 | 1u << 16);
    /* the power chip's register f & 7 written with a byte of the frame's, and register f + 1 read back, which the write
       seven frames before set: what the ARM7 answers is read from the FIFO and kept */
    static const uint32_t power[] = {1u << 25 | 0x6400, 1u << 24 | 1u << 16, 1u << 25 | 0x6500, 1u << 24 | 1u << 16};
    uint32_t args[] = {f & 7, f * 37 & 0xFF, (f + 1) & 7, 0};
    for (unsigned i = 0; i < 4; ++i) nds_io_write(b, IO + 0x188, 4, 8 | (power[i] | args[i]) << 6);
    for (unsigned k = 0; !(nds_io_read(b, IO + 0x184, 2) & 0x100); ++k)
        snx_st_le32(w + REPLIES + (f * 4 + k) % 256 * 4, nds_io_read(b, IO + 0x100000, 4));
    if (f % 4) return;
    /* the square wave on channel 8 at a pitch of the frame's, and once the noise on channel 14, then both started */
    uint32_t list = LISTS + (f / 4 % 2) * 0x80, at = list;
    command(w, at, at + 0x20, 0xF, 8, 100, (0x400u + f * 16) << 8 | 64, 4);
    at += 0x20;
    if (!f) {
        command(w, at, at + 0x20, 0x10, 14, 40, 0x2000u << 8 | 32, 0);
        at += 0x20;
    }
    command(w, at, 0, 0xC, f ? 1u << 8 : 1u << 8 | 1u << 14, 0, 0, 0);
    nds_io_write(b, IO + 0x188, 4, 7 | list << 6);
    nds_io_write(b, IO + 0x188, 4, 7);
}

/* What the frames gave: each one's picture, its sound, and a hash of the VRAM banks after it (where the display
   captures land) */
typedef struct {
    uint32_t* picture;
    uint32_t samples[FRAMES];
    uint64_t vram[FRAMES];
    int16_t* sound;
} Output;
static int output_new(Output* o) {
    *o = (Output){0};
    o->picture = malloc((size_t)FRAMES * PICTURE * 4 + (size_t)FRAMES * SAMPLES * 2 * sizeof(int16_t));
    o->sound = o->picture ? (int16_t*)(o->picture + (size_t)FRAMES * PICTURE) : NULL;
    return o->picture ? 0 : -1;
}
/* frames of the machine from frame first on, each kept in out when it isn't NULL: 0, or 1 having said why */
static int run(Machine* m, unsigned first, unsigned frames, Output* out) {
    char error[512] = "";
    for (unsigned f = 0; f < frames; ++f) {
        stimulus(m->board, first + f);
        if (nds_cpu_run_frame(m->cpu, error, sizeof error)) {
            fprintf(stderr, "nds_state: the machine stopped: %s\n", error);
            return 1;
        }
        int16_t sound[SAMPLES * 2];
        nds_spu_run(m->board);
        uint32_t samples = nds_spu_take(m->board, sound, SAMPLES);
        if (!out) continue;
        CHECK(nds_vk_frame_read((unsigned char*)(out->picture + (size_t)f * PICTURE)));
        out->samples[f] = samples;
        out->vram[f] = SNX_FNV_BASIS;
        for (unsigned bank = 0; bank < NDS_VRAM_BANKS; ++bank)
            out->vram[f] = snx_fnv1a(out->vram[f], m->board->mem.vram[bank].view, nds_vram_bank_size[bank]);
        memcpy(out->sound + (size_t)f * SAMPLES * 2, sound, samples * 2 * sizeof *sound);
    }
    return 0;
}
/* the two runs' frames word for word: 0, or 1 naming the first that differs */
static int same_output(const char* what, const Output* a, const Output* b) {
    for (unsigned f = 0; f < FRAMES; ++f) {
        const uint32_t *pa = a->picture + (size_t)f * PICTURE, *pb = b->picture + (size_t)f * PICTURE;
        for (size_t i = 0; i < PICTURE; ++i)
            if (pa[i] != pb[i]) {
                fprintf(stderr, "nds_state: %s: frame %u's pixel %zu is %08X, the run that went on had %08X\n", what, f,
                        i, pb[i], pa[i]);
                return 1;
            }
        if (a->vram[f] != b->vram[f]) {
            fprintf(stderr, "nds_state: %s: the VRAM after frame %u differs from the run that went on\n", what, f);
            return 1;
        }
        if (a->samples[f] != b->samples[f]) {
            fprintf(stderr, "nds_state: %s: frame %u gave %u samples, the run that went on %u\n", what, f,
                    b->samples[f], a->samples[f]);
            return 1;
        }
        const int16_t *sa = a->sound + (size_t)f * SAMPLES * 2, *sb = b->sound + (size_t)f * SAMPLES * 2;
        for (size_t i = 0; i < a->samples[f] * 2; ++i)
            if (sa[i] != sb[i]) {
                fprintf(stderr, "nds_state: %s: frame %u's sample %zu is %d, the run that went on had %d\n", what, f, i,
                        sb[i], sa[i]);
                return 1;
            }
    }
    return 0;
}
/* every section of two states, byte for byte: 0, or 1 naming the first that differs and where */
static int same_state(const char* what, const char* a, const char* b) {
    static const char* const tags[] = {"BOARD", "CPU", "MEMORY", "GPU3D", "SPU", "PXI", "SOUND"};
    SnxStateHeader ha, hb;
    char why[256] = {0};
    StateReader *ra = sr_open(a, &ha, why, sizeof why), *rb = ra ? sr_open(b, &hb, why, sizeof why) : NULL;
    int r = !ra || !rb;
    if (r) fprintf(stderr, "nds_state: %s: %s\n", what, why);
    for (size_t t = 0; !r && t < sizeof tags / sizeof *tags; ++t) {
        uint64_t sa = 0, sb = 0;
        if (sr_begin(ra, tags[t], &sa) || sr_begin(rb, tags[t], &sb) || sa != sb || sr_left(ra) != sr_left(rb)) {
            fprintf(stderr, "nds_state: %s: the %s sections differ in their schema or size\n", what, tags[t]);
            r = 1;
            break;
        }
        uint64_t n = sr_left(ra);
        uint8_t *pa = malloc(n ? n : 1), *pb = malloc(n ? n : 1);
        if (!pa || !pb || sr_get(ra, pa, n) || sr_get(rb, pb, n)) r = 1;
        for (uint64_t i = 0; !r && i < n; ++i)
            if (pa[i] != pb[i]) {
                fprintf(stderr, "nds_state: %s: the %s sections differ first at byte %llu (%02X, not %02X)\n", what,
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
/* a copy of the file at from, its byte at offset xored with flip, or the file cut to its first cut bytes */
static int copy_changed(const char* from, const char* to, long long offset, uint8_t flip, size_t cut) {
    size_t size = 0;
    uint8_t* bytes = (uint8_t*)snx_read_file(from, &size);
    if (!bytes) return -1;
    if (cut) size = cut;
    if (flip) bytes[offset] ^= flip;
    int r = snx_write_file(to, bytes, size);
    free(bytes);
    return r;
}
/* a load of path refused, its reason holding each of the texts, and the machine as it was: 0, or 1 having said why */
static int refused(Machine* m, const char* dir, const char* path, const char* const* texts) {
    char why[512] = {0}, *before = snx_format("%s/nds_state_before.snxstate", dir),
         *after = snx_format("%s/nds_state_after.snxstate", dir);
    CHECK(before && after);
    CHECK(!snx_nds_state_save(m->board, m->cpu, before, NULL, why, sizeof why));
    if (!snx_nds_state_load(m->board, m->cpu, path, why, sizeof why)) {
        fprintf(stderr, "nds_state: %s loaded\n", path);
        return 1;
    }
    printf("refused %s: %s\n", path, why);
    for (; *texts; ++texts)
        if (!strstr(why, *texts)) {
            fprintf(stderr, "nds_state: the refusal of %s says \"%s\", without \"%s\"\n", path, why, *texts);
            return 1;
        }
    CHECK(!snx_nds_state_save(m->board, m->cpu, after, NULL, why, sizeof why));
    if (same_state("a refused load", before, after)) return 1;
    remove(before);
    remove(after);
    free(before);
    free(after);
    return 0;
}

int main(int argc, char** argv) {
    char why[512] = "";
    if (!nds_vk_init("nds state test", true, why, sizeof why)) {
        printf("nds_state: skipped, %s\n", why);
        return 77;
    }
    CHECK(nds_vk_render3d_init(why, sizeof why) && nds_vk_frame_init(why, sizeof why) &&
          nds_vk_present_init(why, sizeof why));
    const char* dir = argc > 1 ? argv[1] : ".";
    char *image = snx_format("%s/nds_state.nds", dir), *other = snx_format("%s/nds_state_other.nds", dir),
         *saved = snx_format("%s/nds_state_a.snxstate", dir), *went_on = snx_format("%s/nds_state_b.snxstate", dir),
         *again = snx_format("%s/nds_state_c.snxstate", dir), *fresh = snx_format("%s/nds_state_d.snxstate", dir),
         *changed = snx_format("%s/nds_state_x.snxstate", dir);
    CHECK(image && other && saved && went_on && again && fresh && changed);
    CHECK(nds_test_image(image, program, PROGRAM_WORDS));
    /* another game: the same program with a word more */
    uint32_t longer[PROGRAM_WORDS + 1];
    memcpy(longer, program, sizeof program);
    longer[PROGRAM_WORDS] = 0xE1A00000; /* mov r0, r0 */
    CHECK(nds_test_image(other, longer, PROGRAM_WORDS + 1));
    Output ref, loaded, alone;
    CHECK(!output_new(&ref) && !output_new(&loaded) && !output_new(&alone));
    Machine m;
    if (machine_open(&m, image) || run(&m, 0, WARM, NULL)) return 1;
    uint8_t* thumb = malloc((size_t)SNX_STATE_THUMB_W * SNX_STATE_THUMB_H * 4);
    CHECK(thumb);
    for (size_t i = 0; i < (size_t)SNX_STATE_THUMB_W * SNX_STATE_THUMB_H * 4; ++i) thumb[i] = (uint8_t)(i * 5);
    uint64_t t0 = plat_ns();
    CHECK(!snx_nds_state_save(m.board, m.cpu, saved, thumb, why, sizeof why));
    uint64_t t1 = plat_ns();
    if (run(&m, WARM, FRAMES, &ref)) return 1;
    CHECK(!snx_nds_state_save(m.board, m.cpu, went_on, NULL, why, sizeof why));
    /* the program counted its frames, the ARM7 answered, the frames moved and the sound played */
    CHECK(snx_ld_le32(m.board->mem.base + 0x02100014) > WARM);
    CHECK((snx_ld_le32(m.board->mem.base + REPLIES + (WARM + FRAMES - 1) * 4 % 256 * 4) & 0x1F) == 8); /* PXI_POWER */
    CHECK(memcmp(ref.picture, ref.picture + PICTURE, PICTURE * 4));
    {
        size_t loud = 0;
        for (unsigned f = 0; f < FRAMES; ++f)
            for (size_t i = 0; i < ref.samples[f] * 2; ++i) loud += ref.sound[(size_t)f * SAMPLES * 2 + i] != 0;
        CHECK(loud > (size_t)FRAMES * 100);
    }
    uint64_t t2 = plat_ns();
    if (snx_nds_state_load(m.board, m.cpu, saved, why, sizeof why)) {
        fprintf(stderr, "nds_state: %s\n", why);
        return 1;
    }
    uint64_t t3 = plat_ns();
    if (run(&m, WARM, FRAMES, &loaded) || same_output("loaded in the same machine", &ref, &loaded)) return 1;
    CHECK(!snx_nds_state_save(m.board, m.cpu, again, NULL, why, sizeof why));
    if (same_state("loaded in the same machine", went_on, again)) return 1;
    uint64_t identity = m.rom.identity;
    machine_close(&m);
    /* a fresh machine that ran 7 frames of its own: nothing of it stays (the sound driver is the process's, so one
       machine at a time) */
    Machine n;
    if (machine_open(&n, image) || run(&n, 0, 7, NULL)) return 1;
    if (snx_nds_state_load(n.board, n.cpu, saved, why, sizeof why)) {
        fprintf(stderr, "nds_state: %s\n", why);
        return 1;
    }
    if (run(&n, WARM, FRAMES, &alone) || same_output("loaded in a fresh machine", &ref, &alone)) return 1;
    CHECK(!snx_nds_state_save(n.board, n.cpu, fresh, NULL, why, sizeof why));
    if (same_state("loaded in a fresh machine", went_on, fresh)) return 1;
    {
        SnxStateHeader h;
        uint8_t* back = NULL;
        size_t size = 0;
        CHECK(!snx_state_peek(saved, &h, &back, why, sizeof why));
        CHECK(h.abi == SNX_NDS_STATE_VERSION && h.title_id == identity && !strcmp(h.game, n.rom.title));
        CHECK(h.guest_ns > (uint64_t)WARM * 16000000 && h.guest_ns < (uint64_t)(WARM + 1) * 17000000);
        CHECK(h.thumb_w == SNX_STATE_THUMB_W && back &&
              !memcmp(back, thumb, (size_t)SNX_STATE_THUMB_W * SNX_STATE_THUMB_H * 4));
        free(back);
        void* bytes = snx_read_file(saved, &size);
        CHECK(bytes);
        free(bytes);
        printf("a DS state: %zu bytes with its %dx%d thumbnail, saved in %.1f ms, loaded in %.1f ms\n", size,
               SNX_STATE_THUMB_W, SNX_STATE_THUMB_H, (double)(t1 - t0) / 1e6, (double)(t3 - t2) / 1e6);
    }
    {
        size_t size = 0;
        uint8_t* bytes = (uint8_t*)snx_read_file(saved, &size);
        CHECK(bytes && size > SNX_STATE_HEADER);
        uint64_t table = snx_ld_le64(bytes + 48); /* state_header.c: the section table's offset */
        free(bytes);
        const char* const version[] = {"version 9", "version 1", NULL};
        CHECK(!copy_changed(saved, changed, 12, 1 ^ 9, 0));
        if (refused(&n, dir, changed, version)) return 1;
        const char* const layout[] = {"BOARD", "another layout", NULL};
        CHECK(!copy_changed(saved, changed, (long long)table + 8, 0x5A, 0));
        if (refused(&n, dir, changed, layout)) return 1;
        const char* const damaged[] = {"CAPTURE", "damaged", NULL};
        CHECK(!copy_changed(saved, changed, (long long)table - 6, 0x40, 0));
        if (refused(&n, dir, changed, damaged)) return 1;
        const char* const cut[] = {"not written to its end", NULL};
        CHECK(!copy_changed(saved, changed, 0, 0, (size_t)table + 20));
        if (refused(&n, dir, changed, cut)) return 1;
    }
    machine_close(&n);
    {
        Machine o;
        char hash[24], mine[24];
        if (machine_open(&o, other) || run(&o, 0, 3, NULL)) return 1;
        snprintf(hash, sizeof hash, "%016llX", (unsigned long long)identity);
        snprintf(mine, sizeof mine, "%016llX", (unsigned long long)o.rom.identity);
        const char* const game[] = {"another game's", hash, mine, NULL};
        if (refused(&o, dir, saved, game)) return 1;
        machine_close(&o);
    }
    free(ref.picture);
    free(loaded.picture);
    free(alone.picture);
    free(thumb);
    remove(image), remove(other), remove(saved), remove(went_on), remove(again), remove(fresh), remove(changed);
    free(image), free(other), free(saved), free(went_on), free(again), free(fresh), free(changed);
    nds_vk_present_free();
    nds_vk_frame_free();
    nds_vk_render3d_free();
    nds_vk_free();
    return 0;
}
