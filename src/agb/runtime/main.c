/* SPDX-License-Identifier: MPL-2.0
   Copyright 2026 the mGBA contributors
   Copyright 2026 the ScarletNX contributors */
/* scarletnx-agb: the AGB (Game Boy Advance) system type. A ROM goes through the ARM recompiler into a
   native module, then runs on the emulated board, the way the NX and GNU ABI system types prepare their
   code before playing. */
#include "agb/board/board.h"
#include "agb/cpu/agb_cpu.h"
#include "agb/cpu/memmap.h"
#include "agb/cpu/recomp.h"
#include "agb/runtime/host.h"
#include "common/utilities/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef SNX_AGB_CC
#define SNX_AGB_CC "cc"
#endif

/* a save the game writes goes on disk this many frames (a second) after its first write, and at the end */
#define SAVE_QUIET_FRAMES 60

static void usage(void) {
    puts("Usage: scarletnx-agb [OPTIONS] ROM.gba\n"
         "Recompiles a Game Boy Advance ROM and plays it on the AGB board, in a window.\n"
         "\n"
         "  -r, --recompile   rebuild the module even when the cache is current\n"
         "  --data DIR        the data folder whose recompiled/ holds the game's folder (default: the user's)\n"
         "  --outdir DIR      the folder of the games, in which the game's folder is made\n"
         "  --fullscreen=1    start fullscreen (F11 switches)\n"
         "  --frames=N        run N frames with no window, sound or pacing, and exit (tests)\n"
         "  --shot=FILE       write the final frame as a PPM image\n"
         "  --registers       print the CPU's registers when the run ends\n"
         "\n"
         "The game's folder, named after the ROM's file, holds its save (<name>.sav) and the recompiled module\n"
         "(<name>.code.so); a <ROM>.sav beside the ROM is read once when the folder has none.");
}

/* The frame the host presents: 240x160 RGB, straight out of the display device. */
static int write_shot(const char* path, const uint32_t* framebuffer) {
    FILE* file = fopen(path, "wb");
    if (!file) return -1;
    fprintf(file, "P6\n%d %d\n255\n", AGB_SCREEN_WIDTH, AGB_SCREEN_HEIGHT);
    for (int i = 0; i < AGB_SCREEN_WIDTH * AGB_SCREEN_HEIGHT; ++i) {
        uint8_t pixel[3] = {(uint8_t)framebuffer[i], (uint8_t)(framebuffer[i] >> 8), (uint8_t)(framebuffer[i] >> 16)};
        fwrite(pixel, 1, 3, file);
    }
    fclose(file);
    return 0;
}

/* the ROM's file name without its folder and its last extension: the game's folder's name */
static char* game_name(const char* rom) {
    const char* leaf = rom;
    for (const char* c = rom; *c; ++c)
        if (*c == '/' || *c == '\\') leaf = c + 1;
    char* name = snx_strdup(leaf);
    char* dot = strrchr(name, '.');
    if (dot && dot != name) *dot = 0;
    return name;
}

/* <ROM>.sav: the ROM's path, its last extension a .sav, where another emulator keeps the game's save */
static char* save_beside(const char* rom) {
    const char *leaf = rom, *dot;
    for (const char* c = rom; *c; ++c)
        if (*c == '/' || *c == '\\') leaf = c + 1;
    dot = strrchr(leaf, '.');
    return snx_format("%.*s.sav", (int)(dot && dot != leaf ? (size_t)(dot - rom) : strlen(rom)), rom);
}

/* The game's folder: in outdir, else the data folder's recompiled/ as the Switch and 3DS drivers make theirs. Made,
   resolved; NULL with a line saying why */
static char* game_folder(const char* data, const char* outdir, const char* argv0, const char* name) {
    char *exe = data || outdir ? NULL : snx_executable(argv0), *dir = exe ? snx_dirname(exe) : NULL;
    char* base = outdir ? NULL : data ? snx_strdup(data) : snx_data_dir(dir);
    char* folder = outdir ? snx_path_join(outdir, name) : base ? snx_recompiled_game(base, name) : NULL;
    char* resolved = folder && !snx_mkdirs(folder) ? snx_absolute(folder) : NULL;
    if (!resolved) {
        char why[256];
        fprintf(stderr, "scarletnx-agb: cannot make the game's folder %s: %s\n", folder ? folder : name,
                snx_os_error(why, sizeof why));
    }
    free(folder);
    free(base);
    free(dir);
    free(exe);
    return resolved;
}

/* The save into the board's save memory from path, at most its kind's size: 1 read, 0 no file, -1 a file it cannot */
static int save_read(AgbBoard* board, const char* path) {
    size_t size = 0;
    unsigned char* bytes;
    if (!snx_is_file(path)) return 0;
    if (!(bytes = snx_read_file(path, &size))) return -1;
    memcpy(board->memory.sram, bytes, size < agb_save_size(board) ? size : agb_save_size(board));
    free(bytes);
    return 1;
}

/* the board's save memory, its kind's size, to path: 0, else -1 with a line saying why */
static int save_write(AgbBoard* board, const char* path) {
    char why[256];
    if (!snx_write_file(path, board->memory.sram, agb_save_size(board))) {
        board->save.dirty = 0;
        return 0;
    }
    fprintf(stderr, "scarletnx-agb: cannot write the save %s: %s\n", path, snx_os_error(why, sizeof why));
    return -1;
}

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    const char *path = NULL, *shot = NULL, *data = NULL, *outdir = NULL;
    long frames = -1;
    int recompile = 0, registers = 0, fullscreen = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            usage();
            return 0;
        }
        if (!strcmp(argv[i], "-r") || !strcmp(argv[i], "--recompile"))
            recompile = 1;
        else if (!strncmp(argv[i], "--frames=", 9))
            frames = strtol(argv[i] + 9, NULL, 0);
        else if (!strncmp(argv[i], "--shot=", 7))
            shot = argv[i] + 7;
        else if (!strcmp(argv[i], "--registers"))
            registers = 1;
        else if (!strcmp(argv[i], "--fullscreen=0") || !strcmp(argv[i], "--fullscreen=1"))
            fullscreen = argv[i][13] == '1';
        else if (!strcmp(argv[i], "--data") || !strcmp(argv[i], "--outdir")) {
            if (++i == argc || !*argv[i]) {
                fprintf(stderr, "scarletnx-agb: %s takes a folder\n", argv[i - 1]);
                return 2;
            }
            *(!strcmp(argv[i - 1], "--data") ? &data : &outdir) = argv[i];
        } else if (!strcmp(argv[i], "--"))
            continue;
        else if (!path && argv[i][0] != '-')
            path = argv[i];
        else {
            fprintf(stderr, "scarletnx-agb: unexpected argument: %s\n", argv[i]);
            return 2;
        }
    }
    if (!path) {
        usage();
        return 2;
    }
    {
        AgbBoard board;
        AgbMap map;
        AgbCpu cpu;
        AgbRecomp recomp;
        AgbHost* host = NULL;
        char error[512] = {0};
        char *name = NULL, *folder = NULL, *base = NULL, *save = NULL;
        unsigned char* image = NULL;
        size_t length = 0;
        int16_t* audio = NULL;
        int status = 1;
        long written_at = -1; /* the frame the game last wrote its save at, -1 for none since it was kept */
        if (!(image = snx_read_file(path, &length))) {
            char why[256];
            fprintf(stderr, "scarletnx-agb: cannot read %s: %s\n", path, snx_os_error(why, sizeof why));
            return 1;
        }
        agb_board_init(&board);
        if (agb_board_insert(&board, image, length, error, sizeof error)) {
            fprintf(stderr, "scarletnx-agb: %s: %s\n", path, error);
            free(image);
            agb_board_destroy(&board);
            return 1;
        }
        free(image);
        printf("ScarletNX - %s (AGB)\n", board.cartridge.title);
        name = game_name(path);
        if (!(folder = game_folder(data, outdir, argv[0], name))) {
            free(name);
            agb_board_destroy(&board);
            return 1;
        }
        base = snx_format("%s/%s.code", folder, name);
        save = snx_format("%s/%s.sav", folder, name);
        if (agb_map_init(&map, &board, error, sizeof error)) {
            fprintf(stderr, "scarletnx-agb: %s\n", error);
            goto board;
        }
        if (!map.mirrored)
            fprintf(stderr,
                    "scarletnx-agb: the host's pages are %zu bytes, past the %u a memory mirror can be a view at: each "
                    "mirror is RAM of its own\n",
                    map.page, AGB_MAP_VIEW_MAX);
        /* the save: the game folder's, else once a .sav beside the ROM that another emulator wrote */
        if (agb_save_size(&board)) {
            int read = save_read(&board, save);
            if (!read) {
                char* beside = save_beside(path);
                if ((read = save_read(&board, beside)) == 1) {
                    printf("Read the save %s, which %s keeps from now on\n", beside, save);
                    board.save.dirty = 1; /* written to the game's folder at the first chance */
                }
                free(beside);
            }
            if (read < 0) {
                char why[256];
                fprintf(stderr, "scarletnx-agb: cannot read the save %s: %s\n", save, snx_os_error(why, sizeof why));
                goto map;
            }
        }
        agb_cpu_init(&cpu, &board, map.base);
        agb_recomp_init(&recomp, base, SNX_AGB_CC);
        if (recompile || agb_recomp_load_cache(&recomp, &board, error, sizeof error)) {
            if (error[0])
                printf("Recompiling %s: %s\n", path, error);
            else
                printf("Recompiling %s\n", path);
            error[0] = 0;
            if (agb_recomp_build(&recomp, &board, error, sizeof error)) {
                fprintf(stderr, "scarletnx-agb: %s\n", error);
                goto done;
            }
            printf("Built %zu blocks\n", recomp.count);
        }
        audio = (int16_t*)malloc(2 * sizeof(int16_t) * AGB_APU_RATE);
        if (!audio) goto done;
        if (frames < 0) {
            char* title = snx_format("ScarletNX - %s (GBA)", board.cartridge.title[0] ? board.cartridge.title : name);
            host = agb_host_open(title, fullscreen, error, sizeof error);
            free(title);
            if (!host) {
                fprintf(stderr, "scarletnx-agb: %s\n", error);
                goto done;
            }
        }
        for (long frame = 0; frames < 0 || frame < frames; ++frame) {
            uint32_t written = 0;
            uint16_t keys = 0;
            if (host) {
                if (!agb_host_poll(host, &keys)) break;
                agb_board_keys(&board, keys);
            }
            if (agb_cpu_run(&board, &cpu, &recomp, AGB_SCANLINE_CYCLES * AGB_FRAME_LINES, audio, AGB_APU_RATE, &written,
                            error, sizeof error)) {
                fprintf(stderr, "scarletnx-agb: %s\n", error);
                goto done;
            }
            if (host) agb_host_frame(host, board.ppu.framebuffer, audio, written);
            if (board.save.dirty && written_at < 0) written_at = frame;
            if (written_at >= 0 && frame - written_at >= SAVE_QUIET_FRAMES && board.save.dirty &&
                agb_save_size(&board)) {
                save_write(&board, save);
                written_at = -1;
            }
        }
        if (shot && write_shot(shot, board.ppu.framebuffer)) fprintf(stderr, "scarletnx-agb: cannot write %s\n", shot);
        if (registers) {
            for (int i = 0; i < 16; ++i) printf("r%d=%08X ", i, cpu.arm.r[i]);
            printf("cpsr=%08X\n", cpu.arm.cpsr);
        }
        status = 0;
    done:
        agb_host_close(host);
        if (board.save.dirty && agb_save_size(&board) && save_write(&board, save)) status = 1;
        error[0] = 0;
        if (recomp.dirty)
            printf("Keeping the %zu blocks this run compiled in %s.so for the next one\n", recomp.compiled, base);
        if (agb_recomp_save(&recomp, &board, error, sizeof error))
            fprintf(stderr, "scarletnx-agb: cannot save the recompiled ROM: %s\n", error);
        free(audio);
        agb_recomp_free(&recomp);
    map:
        agb_map_destroy(&map, &board);
    board:
        agb_board_destroy(&board);
        free(save);
        free(base);
        free(folder);
        free(name);
        return status;
    }
}
