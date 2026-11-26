/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* scarletnx-nds: the DS system type. A card image's ARM9 code is compiled ahead of time into a native module
   (aot/), as a 3DS game's is, and runs on the board (board/), whose second processor, BIOS and firmware are answered
   by the runtime (hle/, bios/) and whose picture is drawn on the GPU (gpu/). A game is its image, read where it lies:
   there is nothing to extract and no update to add, so its folder is made as a GBA game's is. */
#include "nds/runtime/main.h"
#include "common/utilities/game_log.h"
#include "common/utilities/tracy.h"
#include "common/utilities/menu_window.h"
#include "common/cpu/recompiler/llvm/jit_settings.h"
#include "common/utilities/endian.h"
#include "nds/aot/nds_module.h"
#include "nds/cartridge/key1.h"
#include "nds/board/nds.h"
#include "nds/cpu/nds_cpu.h"
#include "nds/cpu/recomp.h"
#include "nds/runtime/host.h"
#include "common/platform/time.h"
#include "common/utilities/build_info.h"
#include "common/utilities/cli.h"
#include "common/utilities/config_file.h"
#include "common/utilities/common.h"
#include "common/utilities/error_kind.h"
#include "common/utilities/game_ini.h"
#include "common/utilities/script.h"
#include "common/utilities/vpad_consoles.h"
#include "common/utilities/vpad_sdl.h"
#include "common/utilities/console_language.h"
#include "common/utilities/console_settings.h"
#include "common/utilities/state_header.h"
#include "nds/runtime/state.h"
#include "common/online/wfc.h"
#include "snx_settings.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define FRAME_MS ((double)NDS_FRAME_CYCLES * 1000.0 / NDS_CLOCK)
/* a script's keys: KEYINPUT's ten, then X and Y, then the pen (0x1000, at the place 0xXXYY0000 gives) */
#define SCRIPT_KEYS 0xFFFF1FFFull
/* a save the game writes goes on disk this many frames (a second) after its last write, and at the end */
#define SAVE_QUIET_FRAMES 60

static const unsigned nds_cli = SNX_CLI_BIT(SNX_CLI_RECOMPILE) | SNX_CLI_BIT(SNX_CLI_GAME) | SNX_CLI_BIT(SNX_CLI_DATA) |
                                SNX_CLI_BIT(SNX_CLI_OUTDIR) | SNX_CLI_BIT(SNX_CLI_OUTPUT) |
                                SNX_CLI_BIT(SNX_CLI_FRAMES) | SNX_CLI_BIT(SNX_CLI_SECONDS) | SNX_CLI_BIT(SNX_CLI_SHOT) |
                                SNX_CLI_BIT(SNX_CLI_SCRIPT) | SNX_CLI_BIT(SNX_CLI_FULLSCREEN) |
                                SNX_CLI_BIT(SNX_CLI_HEADLESS) | SNX_CLI_BIT(SNX_CLI_HELP) |
                                SNX_CLI_BIT(SNX_CLI_FROM_LAUNCHER);

static void usage(FILE* out) {
    fputs("Usage: scarletnx-nds [OPTIONS] GAME.nds\n"
          "       scarletnx-nds [OPTIONS] --game=recompiled/<name>\n"
          "       <name> [OPTIONS]   (the game's copy of the runtime, in its folder)\n"
          "Recompiles a DS card image's ARM9 code ahead of time and plays it, both screens in a window.\n"
          "\n",
          out);
    snx_cli_help(out, nds_cli);
    fputs(
        "NDS options:\n"
        "  --press-len=MS         How long each scripted press is held (default 300)\n"
        "  --controls=0|1|2       The on-screen controls: 1 shown, 0 hidden, 2 (Auto, the default) shown on a touch\n"
        "                         screen while no gamepad is connected. --controls-scale=N, --controls-snap=0|1,\n"
        "                         --controls-dpad-slide=0|1, --controls-haptics=0|1 and --controls-<ID>-shown=0|1\n"
        "                         (ID one of a, b, x, y, l, r, plus for Start, minus for Select, dpad) as a Switch\n"
        "                         game's\n"
        "  --touch=0|1|2          The lower screen's touch: 2 (default) a finger or the mouse's left button, 1 a\n"
        "                         finger, 0 none\n"
        "  --aot-threads=N        The threads a recompile may take (0: the system's choice)\n"
        "  --aot-threads-most=N   The most threads --aot-threads=0 takes (0: the computer's cores, a quarter left\n"
        "                         free); beside --aot-threads=N it changes nothing, which the recompile says\n"
        "  --shot-every=N         With --shot PATH, also write PATH.<frame>.ppm every N frames\n"
        "  --aot=0                Do not compile the game first when it has no module: every block compiles as the\n"
        "                         run reaches it (a quick look at a game; -r compiles it later, from the hints)\n"
        "  --wav=PATH             Write the run's sound to PATH, 16 bits stereo\n"
        "  --dump_ram=PATH        Write 0x01FF8000 to 0x02FFFFFF to PATH when the run ends\n"
        "  --trace                Say each register access and ARM7 request the board does not know\n"
        "  --nds-wfc=SERVICE      The WFC service the firmware's first Wi-Fi connection asks for (off, auto: the one\n"
        "                         that answers first, custom, kaeru, wiilink, wiimmfi, wiilink_wfc, altwfc, newwfc,\n"
        "                         skytemple, diyfi, sudomemo; default off, or settings.ini's [Online] nds_wfc)\n"
        "  --nds-wfc-dns=IPV4     The DNS server --nds-wfc=custom sets\n"
        "\n"
        "--nds-aot, --nds-trace, --nds-wav and --nds-dump-ram are the settings of the launcher's DS group, the same\n"
        "as --aot, --trace, --wav and --dump_ram: the last given counts.\n"
        "--press-len, --aot-threads, --aot-threads-most, --shot-every, --nds-wfc and --nds-wfc-dns are settings:\n"
        "each with an _ for each - (--press_len=MS), the older spelling, is still read, and said once.\n"
        "\n"
        "F2, or Android's Back, edits the on-screen controls' layout: a control dragged moves, and one tapped is\n"
        "sized from the bar; Done, Esc, F2 or Back ends it, and <name>.ini keeps it. Back twice quits.\n"
        "\n"
        "--frames runs that many of the console's frames and --seconds that much of its time, then ends. --headless\n"
        "opens no window. --shot writes the last frame, both screens, as a PPM image. --script=MS:KEYS,... presses\n"
        "keys MS after the run's start, KEYS in hex: KEYINPUT's bits (1 A, 2 B, 4 Select, 8 Start, 10 Right, 20\n"
        "Left, 40 Up, 80 Down, 100 R, 200 L), 400 X, 800 Y, and 1000 the pen, at the lower screen's pixel the\n"
        "top 16 bits give (XXYY0000).\n"
        "\n"
        "The game's folder, named after the image's file, holds the compiled module (<name>_code.so), the blocks\n"
        "runs compiled at run time (<name>.hints, which a recompile, -r, starts from), the save (<name>.sav), a\n"
        "copy of the runtime that plays it (<name>) and <name>.ini, which names the image and the console.\n",
        out);
}

/* The WFC settings the launcher keeps in settings.ini's [Online] of the data folder data, each where the command
   line gave none (given: nds_wfc's bit 1, nds_wfc_dns's bit 2). The values are checked where the command line's are */
/* a key of settings.ini's [Online] the command line gave none: read as the command line's is, its value said where it
   is none of the setting's */
static void network_take(void* ctx, const char* section, const char* key, const char* value, int line) {
    unsigned given = *(const unsigned*)ctx;
    const IniOpt* o = strcmp(section, "Online") ? NULL : ini_find(g_nds_network_ini, key);
    (void)line;
    if (o && !(given & (!strcmp(key, "nds_wfc") ? 1u : 2u))) ini_read(o, value);
}
static void network_settings(const char* data, unsigned given) {
    char *config = snx_menu_dir(data, SNX_USER_CONFIG), *path = config ? snx_path_join(config, "settings.ini") : NULL;
    if (path) snx_config_keys(path, network_take, &given); /* one it cannot read said, once */
    free(path);
    free(config);
}

/* nds_wfc=auto: every service's DNS server asked at once, the first to answer taken, each one's time said. 1 with dns
   its server's, else 0 (none answered: the connections stay unset, as with off) */
static int wfc_fastest(uint8_t dns[4]) {
    enum { MOST = 32, WAIT_MS = 1500 };
    uint8_t servers[MOST][4];
    int ms[MOST];
    char line[1024], why[512];
    size_t count = snx_online_wfc_preset_count(), at = 0;
    if (count > MOST) count = MOST;
    for (size_t i = 0; i < count; ++i) memcpy(servers[i], snx_online_wfc_preset_at(i)->dns, 4);
    int best = snx_online_wfc_fastest(servers, NULL, count, WAIT_MS, ms, why, sizeof why);
    for (size_t i = 0; i < count && at < sizeof line; ++i)
        at += (size_t)(ms[i] >= 0 ? snprintf(line + at, sizeof line - at, "%s%s %d ms", i ? ", " : "",
                                             snx_online_wfc_preset_at(i)->name, ms[i])
                                  : snprintf(line + at, sizeof line - at, "%s%s no answer", i ? ", " : "",
                                             snx_online_wfc_preset_at(i)->name));
    printf("WFC: nds_wfc=auto asked every service's DNS server: %s\n", line);
    if (best < 0) {
        fprintf(stderr, "scarletnx-nds: nds_wfc=auto: %s: the firmware's Wi-Fi connections are not set, as with off\n",
                why);
        return 0;
    }
    memcpy(dns, servers[best], 4);
    printf("WFC: nds_wfc=auto takes %s, the first to answer (%d ms)\n", snx_online_wfc_preset_at((size_t)best)->name,
           ms[best]);
    return 1;
}

static char* game_name(const char* rom) {
    char* name = snx_strdup(snx_path_leaf(rom));
    char* dot = strrchr(name, '.');
    if (dot && dot != name) *dot = 0;
    return name;
}

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

/* The folder of the game this program is the copy of: named after the game, beside its module. NULL for the runtime
   under its own name */
static char* own_game(const char* argv0) {
    char *exe = snx_executable(argv0), *dir = exe ? snx_dirname(exe) : NULL, *name = exe ? program_name(exe) : NULL;
    char* module = dir && name ? snx_format("%s/%s" NDS_MODULE_SUFFIX, dir, name) : NULL;
    char* folder = name && strncmp(name, "scarletnx", 9) && module && snx_is_file(module) ? snx_strdup(dir) : NULL;
    free(module);
    free(name);
    free(dir);
    free(exe);
    return folder;
}

bool snx_nds_game_name(const char* argv0) {
    char *folder = own_game(argv0), *exe = folder ? snx_executable(argv0) : NULL, *name = exe ? program_name(exe) : NULL;
    bool nds = name && snx_game_system(folder, name) == SNX_SYSTEM_NDS;
    free(name);
    free(exe);
    free(folder);
    return nds;
}

/* the DS's ARM7 BIOS an encrypted secure area is decrypted with: bios7.bin of the data folder, where Install Keys puts
   it. The run's (--data), else for an image named the program's (snx_data_dir, as its game's folder is made in), else
   the one the game's folder is in (snx_menu_dir, as a game finds its launcher's files). NULL when none can be named */
static char* bios7_path(const char* argv0, const char* data, const char* folder, bool image) {
    char *exe = !data && image ? snx_executable(argv0) : NULL, *dir = exe ? snx_dirname(exe) : NULL;
    char* home = data ? snx_strdup(data) : image ? snx_data_dir(dir) : snx_menu_dir(folder, SNX_USER_DATA);
    char* path = home ? snx_path_join(home, NDS_BIOS7_FILE) : NULL;
    free(home);
    free(dir);
    free(exe);
    return path;
}

static void save_load(NdsBoard* board, const char* path, const char* rom_path) {
    size_t size = 0;
    unsigned char* bytes = snx_is_file(path) ? snx_read_file(path, &size) : NULL;
    if (!bytes) { /* once, a save another emulator keeps beside the image */
        const char* dot = strrchr(rom_path, '.');
        char* beside = snx_format("%.*s.sav", (int)(dot ? dot - rom_path : (ptrdiff_t)strlen(rom_path)), rom_path);
        if (beside && snx_is_file(beside) && (bytes = snx_read_file(beside, &size))) {
            printf("Read the save %s, which %s keeps from now on\n", beside, path);
            board->save_dirty = 1;
        }
        free(beside);
    }
    if (!bytes) return;
    if (size > 0x100000) size = 0x100000;
    board->save = malloc(0x100000);
    if (board->save) {
        memset(board->save, 0xFF, 0x100000);
        memcpy(board->save, bytes, size);
        board->save_size = (uint32_t)size;
    }
    free(bytes);
}

static void save_store(NdsBoard* board, const char* path) {
    if (!board->save || !board->save_size) return;
    /* a whole power of two, as the card's memory is */
    uint32_t size = 512;
    while (size < board->save_size) size *= 2;
    if (snx_write_file(path, board->save, size)) {
        char why[256];
        fprintf(stderr, "scarletnx-nds: cannot write the save %s: %s\n", path, snx_os_error(why, sizeof why));
        return;
    }
    board->save_dirty = 0;
}

/* the DS's pair as the window's save states call it */
typedef struct {
    NdsBoard* board;
    NdsCpu* cpu;
} StateGame;
static int state_save(void* game, const char* path, const uint8_t* thumb, char* why, size_t whysize) {
    StateGame* g = (StateGame*)game;
    return snx_nds_state_save(g->board, g->cpu, path, thumb, why, whysize);
}
static int state_load(void* game, const char* path, char* why, size_t whysize) {
    StateGame* g = (StateGame*)game;
    return snx_nds_state_load(g->board, g->cpu, path, why, whysize);
}
/* between two frames: a save states' key asked of host, and the unattended saves and loads due at ms of the run's
   clock, both screens shown their picture */
static void states_step(NdsHost* host, NdsBoard* board, NdsCpu* cpu, const char* folder, const char* name, double ms) {
    int ask = nds_host_state_ask(host);
    if (ask < 0 && !snx_vpad_window_states_waiting()) return;
    uint8_t* thumb = malloc((size_t)SNX_STATE_THUMB_W * SNX_STATE_THUMB_H * 4);
    char* prefix = folder ? snx_format("%s/", folder) : NULL;
    StateGame game = {board, cpu};
    SnxVpadStates states = {prefix ? prefix : "./", name, &game, state_save, state_load};
    const uint8_t* picture = thumb && nds_host_thumb(host, thumb) ? thumb : NULL;
    if (ask >= 0) snx_vpad_window_states(ask, &states, picture);
    snx_vpad_window_states_due((uint64_t)ms, &states, picture);
    free(prefix);
    free(thumb);
}

int snx_nds_main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    const char* path = NULL;
    long press_len = 300;
    int threads = 0, most = 0, trace = 0, aot = 1; /* most: aot_threads_most, which the launcher's Gentle queue gives */
    long shot_every = 0;
    /* nds_wfc (bit 1) and nds_wfc_dns (bit 2), given on the command line */
    unsigned wfc_given = 0;
    const char* wav = NULL; /* the run's sound written there, 16 bits stereo */
    const char* dump_ram = NULL; /* main memory written there when the run ends, for a look at what the game holds */
    SnxCli cli;
    vpad_console(&snx_vpad_nds);
    snx_vpad_sdl_settings(SNX_SYSTEM_NDS);
    snx_cli_init(&cli, "scarletnx-nds", nds_cli);
    for (int i = 1; i < argc; ++i) {
        char error[256];
        int common = snx_cli_take(&cli, argc, argv, &i, error, sizeof error);
        if (common < 0 || common == SNX_CLI_HELP) {
            if (common < 0)
                fprintf(stderr, "%s\n", error);
            else
                usage(stdout);
            snx_cli_free(&cli);
            return common < 0 ? 2 : 0;
        }
        if (common) continue;
        /* the on-screen controls' and the pen's settings, read with the game's own once its folder is known */
        int controls = vpad_arg(argv[i], error, sizeof error);
        if (controls < 0) {
            fprintf(stderr, "scarletnx-nds: %s\n", error);
            snx_cli_free(&cli);
            return 2;
        }
        if (controls) continue;
        /* one of the Switch's settings a DS game does not take, said with why */
        if (snx_console_arg(SNX_SYSTEM_NDS, argv[i], error, sizeof error) < 0) {
            fprintf(stderr, "scarletnx-nds: %s\n", error);
            snx_cli_free(&cli);
            return 2;
        }
        char* end;
        /* the settings it takes, their keys' _ a - or not */
        char key[64];
        const char* value = settings_arg(argv[i], key, sizeof key);
        if (value && !strcmp(key, "press_len")) {
            press_len = strtol(value, &end, 10);
            if (end == value || *end || press_len < 0 || press_len > 0x7FFFFFFF) {
                fprintf(stderr, "scarletnx-nds: --press-len=%s is not a whole number of ms\n", value);
                snx_cli_free(&cli);
                return 2;
            }
        } else if (value && (!strcmp(key, "aot_threads") || !strcmp(key, "aot_threads_most"))) {
            long n = strtol(value, &end, 10);
            if (end == value || *end || n < 0 || n > 1024) {
                fprintf(stderr, "scarletnx-nds: %s=%s is not a whole number from 0 to 1024\n", key, value);
                snx_cli_free(&cli);
                return 2;
            }
            *(strcmp(key, "aot_threads") ? &most : &threads) = (int)n;
        } else if (value && !strcmp(key, "shot_every")) {
            shot_every = strtol(value, &end, 10);
            if (end == value || *end || shot_every < 0) {
                fprintf(stderr, "scarletnx-nds: --shot-every=%s is not a whole number of frames\n", value);
                snx_cli_free(&cli);
                return 2;
            }
        } else if (value && (!strcmp(key, "nds_wfc") || !strcmp(key, "nds_wfc_dns"))) {
            const IniOpt* o = ini_find(g_nds_network_ini, key);
            if (ini_check(o, value, error, sizeof error)) {
                fprintf(stderr, "scarletnx-nds: %s\n", error);
                snx_cli_free(&cli);
                return 2;
            }
            ini_read(o, value);
            wfc_given |= !strcmp(key, "nds_wfc") ? 1u : 2u;
        } else if (value && (ini_find(g_nds_ini, key) || ini_find(g_nds_debug_ini, key))) {
            /* the DS group's settings, each what the option of its own here is: the last given counts */
            const IniOpt* o = ini_find(g_nds_ini, key) ? ini_find(g_nds_ini, key) : ini_find(g_nds_debug_ini, key);
            if (ini_check(o, value, error, sizeof error)) {
                fprintf(stderr, "scarletnx-nds: %s\n", error);
                snx_cli_free(&cli);
                return 2;
            }
            ini_read(o, value);
            if (!strcmp(key, "nds_aot"))
                aot = g_settings.ds_compile_first;
            else if (!strcmp(key, "nds_trace"))
                trace = g_settings.ds_trace;
            else if (!strcmp(key, "nds_wav"))
                wav = g_settings.ds_sound_file[0] ? g_settings.ds_sound_file : NULL;
            else
                dump_ram = g_settings.ds_memory_dump[0] ? g_settings.ds_memory_dump : NULL;
        } else if (!strncmp(argv[i], "--dump_ram=", 11))
            dump_ram = argv[i] + 11;
        else if (!strncmp(argv[i], "--wav=", 6))
            wav = argv[i] + 6;
        else if (!strcmp(argv[i], "--aot=0"))
            aot = 0;
        else if (!strcmp(argv[i], "--aot=1"))
            aot = 1;
        else if (!strcmp(argv[i], "--trace"))
            ++trace; /* twice: each interrupt taken too */
        else if (!strcmp(argv[i], "--"))
            continue;
        else if (!path && argv[i][0] != '-')
            path = argv[i];
        else {
            fprintf(stderr, "scarletnx-nds: unexpected argument: %s\n", argv[i]);
            snx_cli_free(&cli);
            return 2;
        }
    }
    const char *shot = cli.value[SNX_CLI_SHOT], *data = cli.value[SNX_CLI_DATA], *outdir = cli.value[SNX_CLI_OUTDIR];
    const char *output = cli.value[SNX_CLI_OUTPUT], *game = cli.value[SNX_CLI_GAME], *script = cli.value[SNX_CLI_SCRIPT];
    long frames = cli.frames;
    double seconds = cli.seconds;
    int recompile = !!(cli.flags & SNX_CLI_BIT(SNX_CLI_RECOMPILE));
    int headless = !!(cli.flags & SNX_CLI_BIT(SNX_CLI_HEADLESS));
    /* a stop shows its panel where someone is at the window: not a recompile, whose launcher says it, nor a run with
       none or with a set of frames */
    snx_error_note_quiet(recompile || headless || frames >= 0);
    snx_error_launched(cli.value[SNX_CLI_FROM_LAUNCHER] != NULL);
    /* slot 2, the GBA cartridge's: empty, or a Rumble Pak (nds_slot2) */
    int slot2 = nds_slot2_device(g_settings.ds_slot_2_device);
    if (slot2 < 0) {
        fprintf(stderr, "settings: nds_slot2=%s is not none or rumble: it keeps none\n", g_settings.ds_slot_2_device);
        snprintf(g_settings.ds_slot_2_device, sizeof g_settings.ds_slot_2_device, "none");
        slot2 = NDS_SLOT2_NONE;
    }
    /* the WFC service, refused before anything is made when the setting names none */
    uint8_t wfc_dns[4];
    char wfc_why[512];
    network_settings(data, wfc_given);
    int wfc =
        snx_online_wfc_dns(g_settings.ds_wfc_service, g_settings.ds_wfc_dns_server, wfc_dns, wfc_why, sizeof wfc_why);
    if (wfc < 0) {
        fprintf(stderr, "scarletnx-nds: nds_wfc=%s nds_wfc_dns=%s: %s\n", g_settings.ds_wfc_service,
                g_settings.ds_wfc_dns_server, wfc_why);
        snx_cli_free(&cli);
        return 2;
    }
    snx_cli_free(&cli);
    char* own = path || game ? NULL : own_game(argv[0]);
    if ((!path && !game && !own) || (path && game) || (game && (data || outdir || output))) {
        free(own);
        usage(stderr);
        return 2;
    }
    NdsRom rom = {0};
    NdsBoard* board = NULL;
    NdsCpu* cpu = NULL;
    NdsRecomp* recomp = NULL;
    NdsHost* host = NULL;
    SnxPress* presses = NULL;
    size_t press_count = 0;
    char error[512] = {0};
    char *name = NULL, *folder = NULL, *module = NULL, *hints = NULL, *save = NULL, *image = NULL, *ini = NULL;
    char* bios7 = NULL;
    int status = 1;
    if (path) {
        name = game_name(path);
        if (name) snx_error_note_game(name); /* until the card says its title */
        folder = !output ? snx_game_folder(data, outdir, argv[0], name, 1) : snx_mkdirs(output) ? NULL : snx_absolute(output);
        if (!folder) {
            char why[256];
            fprintf(stderr, "scarletnx-nds: cannot make the folder of %s: %s\n", name, snx_os_error(why, sizeof why));
            goto done;
        }
        image = snx_absolute(path);
    } else {
        folder = own ? own : snx_absolute(game);
        own = NULL;
        name = folder ? program_name(folder) : NULL;
    }
    /* the game's log, before any thread of the run's: every line from here in <name>.log too */
    char logged[1600];
    if (folder && name && snx_game_log_start(folder, name, &g_settings.log_size_limit, logged, sizeof logged))
        fprintf(stderr, "scarletnx-nds: %s: the run's lines are on the terminal alone\n", logged);
    ini = folder && name ? snx_format("%s/%s.ini", folder, name) : NULL;
    if (!image && ini && !(image = snx_ini_value(ini, "source")))
        snx_error_note(SNX_ERROR_DUMP_MISSING, "scarletnx-nds", "%s names no image: source= in %s", folder, ini);
    if (!image || !folder || !name) goto done;
    bios7 = bios7_path(argv[0], data, folder, path != NULL);
    if (!nds_rom_open(&rom, image, bios7, error, sizeof error)) {
        snx_error_note(SNX_ERROR_BAD_FILE, "scarletnx-nds", "%s", error);
        goto done;
    }
    /* the header's count of the bytes the game uses: an image that ends before it is a copy that stopped early, and
       the game reads 0xFF where its files should be */
    if (rom.size >= 0x84 && snx_ld_le32(rom.data + 0x80) > rom.size)
        fprintf(stderr, "scarletnx-nds: %s is incomplete: it has %zu bytes of the %u its header counts. The game can stop "
                        "where it reads the rest\n", image, rom.size, (unsigned)snx_ld_le32(rom.data + 0x80));
    snx_jit_settings("scarletnx-nds", g_settings.slp_vectorizer, g_settings.perf_map, g_settings.gdb_jit_symbols);
    module = snx_format("%s/%s" NDS_MODULE_SUFFIX, folder, name);
    hints = snx_format("%s/%s" NDS_HINTS_SUFFIX, folder, name);
    save = snx_format("%s/%s.sav", folder, name);
    if (!module || !hints || !save) goto done;
    if (recompile || (aot && !nds_module_current(module, &rom))) {
        printf("Recompiling %s (offline static AOT)\n", rom.title[0] ? rom.title : name);
        if (nds_aot_compile(&rom, module, hints, threads, most, NULL, NULL, error, sizeof error)) {
            snx_error_note(SNX_ERROR_RECOMPILE, "scarletnx-nds", "%s", error);
            goto done;
        }
    }
    /* the game's copy of the runtime and its .ini, so the launcher's Games plays it as any game made */
    {
#ifdef _WIN32
        char* copy = snx_format("%s/%s.exe", folder, name);
#else
        char* copy = snx_format("%s/%s", folder, name);
#endif
        char* runtime = snx_executable(argv[0]);
        int placed = runtime && copy ? snx_place_runtime(runtime, copy, error, sizeof error) : -1;
        if (placed == 1) printf("The game's runtime is %s\n", copy);
        if (placed < 0) fprintf(stderr, "scarletnx-nds: %s\n", runtime ? error : "cannot name this program");
        error[0] = 0;
        placed = snx_ini_record(ini, "source", image) || snx_record_system(folder, name, SNX_SYSTEM_NDS);
        free(runtime);
        free(copy);
        if (placed) goto done;
    }
    if (recompile) {
        status = 0;
        goto done;
    }
    board = calloc(1, sizeof *board);
    cpu = calloc(1, sizeof *cpu);
    if (!board || !cpu) goto done;
    if (!nds_board_init(board, &rom, error, sizeof error)) {
        snx_error_note(SNX_ERROR_BAD_FILE, "scarletnx-nds", "%s", error);
        goto done;
    }
    board->trace = trace;
    if (!nds_slot2_insert(board, slot2, error, sizeof error)) {
        fprintf(stderr, "scarletnx-nds: %s\n", error);
        goto done;
    }
    if (slot2 == NDS_SLOT2_RUMBLE) printf("Slot 2: a Rumble Pak (nds_slot2=rumble)\n");
    /* the window's and the board's settings, the network's among them, read before the firmware's Wi-Fi */
    vpad_set_say(nds_host_say);
    vpad_settings(folder, name, argc, argv);
    snx_menu_window_open(SNX_SYSTEM_NDS, folder, name);                 /* the game's menu: Back, F2 or a pad's Home */
    if (snx_tracy_console(folder, name, headless || frames >= 0))
        fputs("scarletnx-nds: tracy: no capture this run; it goes on without one\n", stderr);
    if (snx_cli_fullscreen_flag(argc, argv)) g_settings.fullscreen = 1; /* -f's over the settings' */
    /* the player's language, the Switch's setting, as the firmware's: one the DS has none of said */
    int language = snx_language_nds(g_settings.language);
    if (language < 0)
        fprintf(stderr, "settings: language=%d (%s) is no language of the DS: it plays in English\n",
                g_settings.language, snx_language_name(g_settings.language));
    nds_board_language(board, language < 0 ? 1 : language);
    /* offline, as the Switch's network and airplane mode say (the 3DS's too): no Wi-Fi connection set */
    if (wfc && !snx_console_online()) {
        printf("WFC: nds_wfc=%s, and %s: the firmware's Wi-Fi connections are not set\n", g_settings.ds_wfc_service,
               g_settings.airplane_mode ? "airplane_mode=1" : "network=0");
        wfc = 0;
    }
    if (wfc == SNX_ONLINE_WFC_AUTO) wfc = wfc_fastest(wfc_dns);
    if (wfc > 0 && !nds_pxi_wfc(board, wfc_dns, error, sizeof error)) {
        fprintf(stderr, "scarletnx-nds: the firmware's Wi-Fi connection: %s\n", error);
        goto done;
    }
    if (wfc > 0)
        printf("WFC: nds_wfc=%s, the firmware's first Wi-Fi connection (%s) asks the DNS server %u.%u.%u.%u\n",
               g_settings.ds_wfc_service, NDS_WFC_NETWORK, wfc_dns[0], wfc_dns[1], wfc_dns[2], wfc_dns[3]);
    else if (snx_console_online())
        printf("WFC: nds_wfc=%s, the firmware's Wi-Fi connections are not set\n", g_settings.ds_wfc_service);
    save_load(board, save, image);
    recomp = nds_recomp_new(&rom, module, error, sizeof error);
    if (!recomp) goto done;
    if (error[0]) printf("%s: its blocks compile as the run reaches them\n", error);
    error[0] = 0;
    nds_cpu_init(cpu, board, recomp);
    printf("ScarletNX - %s (NDS)\nbuild: %s\n", rom.title[0] ? rom.title : name, snx_build_log_line());
    snx_error_note_game(rom.title[0] ? rom.title : name);
    {
        char run[SNX_RUN_LOG_LINE];
        snx_run_log_line(run, (long long)time(NULL));
        printf("%s\n", run);
    }
    printf("card: %s\n", rom.code);
    {
        char* title = snx_format("ScarletNX - %s (NDS)", rom.title[0] ? rom.title : name);
        host = nds_host_open(board, title, ini, wav, headless || frames >= 0, g_settings.fullscreen && !(seconds > 0),
                             error, sizeof error);
        free(title);
        if (!host) {
            snx_error_note(SNX_ERROR_GPU_START, "scarletnx-nds", "%s", error);
            goto done;
        }
    }
    press_count = snx_script_parse("scarletnx-nds", script, SCRIPT_KEYS, &presses);
    long limit = frames >= 0 ? frames : seconds > 0 ? (long)(seconds * 1000.0 / FRAME_MS + 0.5) : -1;
    long written_at = -1;
    uint32_t told_swi = 0xFFFFFFFFu;
    double began = snx_seconds();
    long frame = 0;
    for (; limit < 0 || frame < limit; ++frame) {
        NdsInput input = {0};
        if (!nds_host_poll(host, &input)) break;
        if (nds_host_held(host)) { /* paused: the board stopped */
            states_step(host, board, cpu, folder, name, (double)frame * FRAME_MS);
            --frame;
            continue;
        }
        uint64_t held = snx_script_held(presses, press_count, (double)frame * FRAME_MS, (uint32_t)press_len);
        /* the pen of the last press that names it */
        if (held & 0x1000) {
            input.touching = 1;
            input.touch_x = (uint16_t)(held >> 24 & 0xFF), input.touch_y = (uint16_t)(held >> 16 & 0xFF);
        }
        input.keys |= (uint16_t)(held & 0xFFF);
        board->keyinput = (uint16_t)(~input.keys & 0x3FF);
        board->extkeyin = (uint16_t)((board->extkeyin & ~0x43u) | (input.keys & 0x400 ? 0 : 1) |
                                     (input.keys & 0x800 ? 0 : 2) | (input.touching ? 0 : 0x40));
        board->touching = input.touching;
        board->touch_x = input.touch_x, board->touch_y = input.touch_y;
        if (nds_cpu_run_frame(cpu, error, sizeof error)) {
            snx_error_note(SNX_ERROR_NOT_YET, "scarletnx-nds", "%s", error);
            fprintf(stderr, "  the blocks before it:");
            for (unsigned i = 0; i < 32; ++i) fprintf(stderr, " %08X", cpu->trail[(cpu->blocks + i) & 31]);
            fputc('\n', stderr);
            goto report;
        }
        if (cpu->unknown_swi_count && cpu->unknown_swi != told_swi) {
            printf("The game called BIOS function 0x%02X at 0x%08X, which the BIOS does not have: it returned at once\n",
                   cpu->unknown_swi, cpu->unknown_swi_at);
            told_swi = cpu->unknown_swi;
        }
        nds_host_frame(host);
        if (nds_host_lost(host, error, sizeof error)) {
            snx_error_note(SNX_ERROR_GPU_LOST, "scarletnx-nds", "%s", error);
            goto report;
        }
        states_step(host, board, cpu, folder, name, (double)frame * FRAME_MS);
        if (shot && shot_every && frame && !(frame % shot_every)) {
            char* each = snx_format("%s.%06ld.ppm", shot, frame);
            if (each) (void)nds_host_shot(host, each);
            free(each);
        }
        if (board->save_dirty && written_at < 0) written_at = frame;
        if (written_at >= 0 && frame - written_at >= SAVE_QUIET_FRAMES && board->save_dirty) {
            save_store(board, save);
            written_at = -1;
        }
    }
    status = 0;
report: {
    double took = snx_seconds() - began;
    uint64_t compiled, hinted, from_module;
    nds_recomp_counts(recomp, &compiled, &hinted, &from_module);
    printf("Ran %ld frames in %.2f s (%.1f a second): %" PRIu64 " blocks, %" PRIu64 " instructions; pc 0x%08X lr 0x%08X "
           "cpsr 0x%08X\n",
           frame, took, took > 0 ? (double)frame / took : 0.0, cpu->blocks, cpu->instructions, cpu->arm.r[15],
           cpu->arm.r[14], cpu->arm.cpsr);
    printf("Blocks reached: %" PRIu64 " from the module, %" PRIu64 " compiled at run time, %u of them Thumb\n", from_module,
           compiled, nds_recomp_thumb(recomp));
    if (compiled) {
        size_t added = nds_recomp_save_hints(recomp, hints);
        printf("Compiled at run time: %" PRIu64 " blocks", compiled);
        if (added) printf(", %zu new in the hints, which a recompile (-r) starts from", added);
        putchar('\n');
    }
    /* 0x01FF8000 to 0x02FFFFFF as the window has it: the ITCM, then main memory and its mirrors, with the DTCM where
       it lies */
    if (dump_ram && snx_write_file(dump_ram, board->mem.base + 0x01FF8000u, 0x01008000u))
        fprintf(stderr, "scarletnx-nds: cannot write %s\n", dump_ram);
    if (trace) {
        for (unsigned i = 0; i < 16; ++i) fprintf(stderr, "r%u=%08X%c", i, cpu->arm.r[i], i % 8 == 7 ? '\n' : ' ');
        for (unsigned e = 0; e < 2; ++e) {
            const uint32_t* w = board->lines[e][100].words;
            fprintf(stderr, "engine %c line 100: DISPCNT %08X BGCNT %08X %08X BLD %08X %08X bright %08X win %08X %08X\n",
                    'A' + e, w[0], w[1], w[2], w[17], w[18], w[19], w[15], w[16]);
        }
        for (unsigned e = 0; e < 2; ++e) {
            const unsigned char* oam = board->mem.oam.view + e * 0x400;
            fprintf(stderr, "OAM %c:", 'A' + e);
            for (unsigned n = 0; n < 128; ++n) {
                unsigned a0 = oam[n * 8] | oam[n * 8 + 1] << 8, a1 = oam[n * 8 + 2] | oam[n * 8 + 3] << 8;
                unsigned a2 = oam[n * 8 + 4] | oam[n * 8 + 5] << 8;
                if ((a0 >> 8 & 3) != 2 && (a0 & 0xFF) < 192) fprintf(stderr, " %u:%04X,%04X,%04X", n, a0, a1, a2);
            }
            fputc('\n', stderr);
        }
        fprintf(stderr, "VRAMCNT");
        for (unsigned i = 0; i < 9; ++i) fprintf(stderr, " %02X", board->vramcnt[i]);
        fprintf(stderr, " POWCNT %04X DISP3DCNT %04X\n", board->powcnt, board->lcd[0][0x30]);
    }
    if (shot && nds_host_shot(host, shot)) fprintf(stderr, "scarletnx-nds: cannot write %s\n", shot);
    snx_tracy_shutdown(); /* a capture's lines before the last */
    if (!status) puts("Exit.");
}
done:
    snx_tracy_shutdown(); /* a stop before the end: the capture keeps what came */
    if (board && board->save_dirty) save_store(board, save);
    nds_host_close(host);
    nds_recomp_free(recomp);
    if (board) nds_board_free(board);
    free(board);
    free(cpu);
    free(presses);
    nds_rom_close(&rom);
    free(save);
    free(hints);
    free(module);
    free(ini);
    free(image);
    free(bios7);
    free(folder);
    free(name);
    free(own);
    return status;
}
