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
#include "common/utilities/game_ini.h"
#include "common/utilities/script.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef SNX_AGB_CC
#define SNX_AGB_CC "cc"
#endif

/* a save the game writes goes on disk this many frames (a second) after its first write, and at the end */
#define SAVE_QUIET_FRAMES 60
/* a frame's time in ms, the console's: the clock a script's presses go by, the same with a window or without */
#define FRAME_MS          ((double)AGB_SCANLINE_CYCLES * AGB_FRAME_LINES * 1000.0 / 16777216.0)
/* KEYINPUT's ten keys, the bits a script's press may hold */
#define SCRIPT_KEYS       0x3FFu

static void usage(void) {
    puts("Usage: scarletnx-agb [OPTIONS] ROM.gba\n"
         "       scarletnx-agb [OPTIONS] --game=recompiled/<name>\n"
         "       <name> [OPTIONS]   (the game's copy of the runtime, in its folder)\n"
         "Recompiles a Game Boy Advance ROM and plays it on the AGB board, in a window.\n"
         "\n"
         "  --game=DIR        play the game in its folder DIR, as its copy of the runtime does\n"
         "  -r, --recompile   compile the module again, without playing\n"
         "  --data DIR        the data folder whose recompiled/ holds the game's folder (default: the user's)\n"
         "  --outdir DIR      the folder of the games, in which the game's folder is made\n"
         "  --fullscreen=1    start fullscreen (F11 switches)\n"
         "  --frames=N        run N frames with no window, sound or pacing, and exit (tests)\n"
         "  --shot=FILE       write the final frame as a PPM image\n"
         "  --script=MS:KEYS,...  press keys at a time of the run, MS after its start, KEYS in hex as KEYINPUT has\n"
         "                    them (1 A, 2 B, 4 Select, 8 Start, 10 Right, 20 Left, 40 Up, 80 Down, 100 R, 200 L)\n"
         "  --press_len=MS    how long each scripted press is held (default 300)\n"
         "  --registers       print the CPU's registers when the run ends\n"
         "\n"
         "The game's folder, named after the ROM's file, holds its save (<name>.sav), the recompiled module\n"
         "(<name>_code.so), the copy of the runtime that plays it from then on (<name>) and <name>.ini, which\n"
         "names the ROM and the console (source=, system=); a <ROM>.sav beside the ROM is read once when the\n"
         "folder has none.");
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

/* a program's file name without its folder, and without .exe on Windows: the game a copy of the runtime is named
   after, dots and all */
static char* program_name(const char* exe) {
    const char* leaf = exe;
    for (const char* c = exe; *c; ++c)
        if (*c == '/' || *c == '\\') leaf = c + 1;
    char* name = snx_strdup(leaf);
#ifdef _WIN32
    size_t n = strlen(name);
    if (n > 4 && !_stricmp(name + n - 4, ".exe")) name[n - 4] = 0;
#endif
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

/* The folder of the game this program is the copy of, as a 3DS or Vita game's copy is: named after the game, beside
   its module <name>_code.so. NULL for the runtime under its own name */
static char* own_game(const char* argv0) {
    char *exe = snx_executable(argv0), *dir = exe ? snx_dirname(exe) : NULL, *name = exe ? program_name(exe) : NULL;
    char* module = dir && name ? snx_format("%s/%s_code.so", dir, name) : NULL;
    char* folder = name && strncmp(name, "scarletnx", 9) && module && snx_is_file(module) ? snx_strdup(dir) : NULL;
    free(module);
    free(name);
    free(dir);
    free(exe);
    return folder;
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
    if (board->save.type == AGB_SAVE_EEPROM && size > agb_save_size(board)) board->save.type = AGB_SAVE_EEPROM8K;
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
    const char *path = NULL, *shot = NULL, *data = NULL, *outdir = NULL, *game = NULL, *script = NULL;
    long frames = -1, press_len = 300;
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
        else if (!strncmp(argv[i], "--script=", 9))
            script = argv[i] + 9;
        else if (!strncmp(argv[i], "--press_len=", 12)) {
            char* end;
            press_len = strtol(argv[i] + 12, &end, 10);
            if (end == argv[i] + 12 || *end || press_len < 0 || press_len > 0x7FFFFFFF) {
                fprintf(stderr, "scarletnx-agb: --press_len=%s is not a whole number of ms\n", argv[i] + 12);
                return 2;
            }
        } else if (!strncmp(argv[i], "--shot=", 7))
            shot = argv[i] + 7;
        else if (!strncmp(argv[i], "--game=", 7) && argv[i][7])
            game = argv[i] + 7;
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
    char* own = path || game ? NULL : own_game(argv[0]); /* the game's copy of the runtime plays its game */
    if ((!path && !game && !own) || (path && game) || (game && (data || outdir))) {
        free(own);
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
        char *name = NULL, *folder = NULL, *base = NULL, *save = NULL, *rom = NULL, *ini = NULL;
        unsigned char* image = NULL;
        size_t length = 0;
        int16_t* audio = NULL;
        int status = 1, played = 0; /* played: the frames ran to their end or the window closed */
        SnxPress* presses = NULL;   /* --script's */
        size_t press_count = 0;
        long written_at = -1; /* the frame the game last wrote its save at, -1 for none since it was kept */
        /* a ROM's game is made in its folder; a game's folder (--game=, or the copy's own) names its ROM */
        if (path) {
            name = game_name(path);
            folder = game_folder(data, outdir, argv[0], name);
            rom = folder ? snx_absolute(path) : NULL;
        } else {
            folder = own ? own : snx_absolute(game);
            own = NULL;
            name = folder ? program_name(folder) : NULL;
        }
        /* the folder's own, as a 3DS or Vita game's: source= and system=, which the launcher and the deploy read */
        ini = folder && name ? snx_format("%s/%s.ini", folder, name) : NULL;
        if (!rom && ini && !(rom = snx_ini_value(ini, "source")))
            fprintf(stderr, "scarletnx-agb: %s names no ROM: source= in %s\n", folder, ini);
        if (!rom || !(image = snx_read_file(rom, &length))) {
            char why[256];
            if (rom) fprintf(stderr, "scarletnx-agb: cannot read %s: %s\n", rom, snx_os_error(why, sizeof why));
            free(ini);
            free(rom);
            free(folder);
            free(name);
            return 1;
        }
        path = rom;
        agb_board_init(&board);
        if (agb_board_insert(&board, image, length, error, sizeof error)) {
            fprintf(stderr, "scarletnx-agb: %s: %s\n", path, error);
            free(image);
            agb_board_destroy(&board);
            free(ini);
            free(rom);
            free(folder);
            free(name);
            return 1;
        }
        free(image);
        base = snx_format("%s/%s_code", folder, name);
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
        /* a recompile builds the module from every block the runs reached, the map's; else the cache is loaded */
        if (recompile ? (agb_recomp_load_map(&recomp, &board, error, sizeof error), 1)
                      : agb_recomp_load_cache(&recomp, &board, error, sizeof error)) {
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
        /* the game's copy of the runtime and its .ini, so the launcher's Games plays it as any game made */
        {
#ifdef _WIN32
            char* copy = snx_format("%s/%s.exe", folder, name);
#else
            char* copy = snx_format("%s/%s", folder, name);
#endif
            char* runtime = snx_executable(argv[0]);
            int placed = runtime ? snx_place_runtime(runtime, copy, error, sizeof error) : -1;
            if (placed == 1) printf("The game's runtime is %s\n", copy);
            if (placed < 0) fprintf(stderr, "scarletnx-agb: %s\n", runtime ? error : "cannot name this program");
            error[0] = 0;
            placed = snx_ini_record(ini, "source", path) || snx_record_system(folder, name, SNX_SYSTEM_GBA);
            free(runtime);
            free(copy);
            if (placed) goto done; /* the record said why */
        }
        if (recompile) { /* R's: the game made, nothing played */
            status = 0;
            goto done;
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
        printf("ScarletNX - %s (GBA)\n", board.cartridge.title[0] ? board.cartridge.title : name);
        press_count = snx_script_parse("scarletnx-agb", script, SCRIPT_KEYS, &presses);
        for (long frame = 0; frames < 0 || frame < frames; ++frame) {
            uint32_t written = 0;
            uint16_t keys = 0;
            if (host && !agb_host_poll(host, &keys)) break;
            keys |= (uint16_t)snx_script_held(presses, press_count, (double)frame * FRAME_MS, (uint32_t)press_len);
            agb_board_keys(&board, keys);
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
        played = 1;
    done:
        agb_host_close(host);
        if (board.save.dirty && agb_save_size(&board) && save_write(&board, save)) status = 1;
        error[0] = 0;
#ifdef SNX_AGB_JIT
        if (recomp.compiled)
            printf("%zu blocks compiled at run time were added to the hints, which a recompile (-r) starts from\n",
                   recomp.compiled);
#else
        if (recomp.dirty)
            printf("Keeping the %zu blocks this run compiled in %s.so for the next one\n", recomp.compiled, base);
#endif
        if (agb_recomp_save(&recomp, &board, error, sizeof error))
            fprintf(stderr, "scarletnx-agb: cannot save the recompiled ROM: %s\n", error);
        if (played && !status) puts("Exit."); /* a run that ended well says so last, as every driver's */
        free(audio);
        free(presses);
        agb_recomp_free(&recomp);
    map:
        agb_map_destroy(&map, &board);
    board:
        agb_board_destroy(&board);
        free(save);
        free(base);
        free(ini);
        free(rom);
        free(folder);
        free(name);
        return status;
    }
}
