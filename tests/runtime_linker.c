/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The runtime's linker beside the game's own (nx/runtime/linker.c): a game of two modules in guest memory, a linker
   whose one PLT slot is nn::init::Start's and an SDK that defines it, with a relative word, a word of its own
   symbol and one of a weak symbol nothing defines. linker_init must take nn::init::Start's dispatch slot only when the module routes the function and the
   game runs on the recompiled code, and the slot's first call must compare the game's memory, which this test
   relocates as the game's linker would, give the slot back, say what it found and run the function.
     runtime_linker          the two agree, one slot not bound yet
     runtime_linker differ   a word the game's linker left differs
   And where the runtime can link the game itself (the module routes nn::ro, whose hooks are on, and the game maps no
   NRO), linker_decide must relocate the game's memory, every slot bound, the main thread must enter
   nn::init::Start with the runtime's three callbacks, the second must run each module's DT_INIT but the rtld's, the
   last module first, the third each DT_FINI in load order, and a name must be found among every module or among the
   ones after a caller's. That game has a third module, which defines nn::init::Start's name too.
     runtime_linker switch */
#include "nx/hle/hook/hook.h"
#include "nx/hle/kernel/kernel.h"
#include "nx/loader/module_abi.h"
#include "nx/loader/relocate.h"
#include "nx/memory/mem.h"
#include "nx/runtime/arm.h"
#include "nx/runtime/linker.h"
#include "nx/runtime/loader.h"
#include "nx/runtime/nce/nce.h"
#include "nx/runtime/platform/log.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define RTLD  0x10000ull
#define SDK   0x20000ull
#define LAST  0x30000ull
#define START (SDK + 0x40)
#define NAME  "_ZN2nn4init5StartEmmPFvvES2_S2_"
enum { MOD0 = 0x80, DYNAMIC = 0xc0, SYMTAB = 0x200, STRTAB = 0x248, RELA = 0x400, JMPREL = 0x500, RELR = 0x600 };
enum { DATA = 0x800, DATA_SIZE = 0x200, MODULE_BYTES = 0x1000 };

static u8 g_memory[0x40000];
u8* g_mem = g_memory;
const SnxNros* g_nros;
static SnxModInfo g_mods[3] = {
    {.name = "rtld", .text_size = 0x80, .base = RTLD, .size = MODULE_BYTES, .data_off = DATA, .data_size = DATA_SIZE},
    {.name = "sdk", .base = SDK, .size = MODULE_BYTES, .data_off = DATA, .data_size = DATA_SIZE},
    {.name = "last", .base = LAST, .size = MODULE_BYTES, .data_off = DATA, .data_size = DATA_SIZE}};
static SnxModule g_module = {.abi = SNX_ABI, .nmods = 2, .mods = g_mods};
const SnxModule* g_mod = &g_module;
const SnxArmModule* g_arm_mod;
static GuestFn g_dispatch[0x30000 / 4];
GuestFn* g_disp = g_dispatch;
u64 g_code_lo = RTLD, g_code_hi = RTLD + 0x30000;

static int failures, g_nce, g_routed, g_ro_routed, g_exported, g_started, g_family_off, g_ran;
static u64 g_order[8];
static char g_log[4096];
static void check(int ok, const char* what) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\nlog:\n%s\n", what, g_log);
        ++failures;
    }
}
void logf_(int lvl, const char* fmt, ...) {
    size_t n = strlen(g_log);
    va_list args;
    (void)lvl;
    va_start(args, fmt);
    vsnprintf(g_log + n, sizeof g_log - n, fmt, args);
    va_end(args);
}
int nce_on(void) { return g_nce; }
int hook_routed(const char* name) { return !strcmp(name, NAME) ? g_routed : g_ro_routed; }
int hook_family_off(const char* family) { return g_family_off && !strcmp(family, LINKER_FAMILY); }
u64 kernel_find_symbol_exact(const char* name) { return g_exported && !strcmp(name, NAME) ? START : 0; }
static void original(CPU* c, u64 entry) {
    (void)c;
    g_started += entry == START;
}
/* a module's DT_INIT and DT_FINI, at 0x48 and 0x4c of it: each says it ran and returns to its caller */
static void ran(CPU* c, u64 entry) {
    if (c->x[30] == 0x5554 && g_ran < 8) g_order[g_ran++] = entry;
    c->pc = c->x[30];
}

/* a module's dynamic table and its one symbol, NAME: defined at value in the module, or imported (0) */
static void module(u64 base, u64 value, int plt) {
    u8* m = GP(base);
    const u64 tags[9][2] = {{6, SYMTAB},       {5, STRTAB},       {10, 3 + sizeof NAME},
                            {7, RELA},         {8, plt ? 0 : 48}, {23, JMPREL},
                            {2, plt ? 24 : 0}, {36, RELR},        {35, plt ? 0 : 8}};
    unsigned i;
    memset(m, 0, MODULE_BYTES);
    snx_st_le32(m + 4, MOD0);
    memcpy(m + MOD0, "MOD0", 4);
    snx_st_le32(m + MOD0 + 4, DYNAMIC - MOD0);
    for (i = 0; i < 9; i++) {
        snx_st_le64(m + DYNAMIC + 16 * i, tags[i][0]);
        snx_st_le64(m + DYNAMIC + 16 * i + 8, tags[i][1]);
    }
    /* DT_INIT and DT_FINI */
    snx_st_le64(m + DYNAMIC + 16 * 9, 12), snx_st_le64(m + DYNAMIC + 16 * 9 + 8, 0x48);
    snx_st_le64(m + DYNAMIC + 16 * 10, 13), snx_st_le64(m + DYNAMIC + 16 * 10 + 8, 0x4c);
    snx_st_le32(m + SYMTAB + 24, 1);
    m[SYMTAB + 24 + 4] = 0x12;
    m[SYMTAB + 24 + 6] = value ? 1 : 0;
    snx_st_le64(m + SYMTAB + 24 + 8, value);
    memcpy(m + STRTAB + 1, NAME, sizeof NAME);
    if (plt) { /* the linker's slot of it, lazy */
        snx_st_le64(m + JMPREL, DATA);
        snx_st_le64(m + JMPREL + 8, (1ull << 32) | 1026);
        snx_st_le64(m + DATA, 0x700);
    } else { /* a word of its own symbol, one of a weak symbol nothing defines ("w"), and a relative one */
        snx_st_le32(m + SYMTAB + 48, 1 + sizeof NAME);
        m[SYMTAB + 48 + 4] = 0x22;
        memcpy(m + STRTAB + 1 + sizeof NAME, "w", 2);
        snx_st_le64(m + RELA, DATA);
        snx_st_le64(m + RELA + 8, (1ull << 32) | 257);
        snx_st_le64(m + RELA + 24, DATA + 0x10);
        snx_st_le64(m + RELA + 32, (2ull << 32) | 1025);
        snx_st_le64(m + DATA + 0x10, 0x999);
        snx_st_le64(m + RELR, DATA + 8);
        snx_st_le64(m + DATA + 8, 0x123);
    }
}
static void game(void) {
    memset(g_dispatch, 0, sizeof g_dispatch);
    module(RTLD, 0, 1);
    module(SDK, 0x40, 0);
    module(LAST, 0x44, 0);
    g_disp[(START - g_code_lo) / 4] = original;
    g_disp[(RTLD + 0x48 - g_code_lo) / 4] = g_disp[(RTLD + 0x4c - g_code_lo) / 4] = ran;
    g_disp[(SDK + 0x48 - g_code_lo) / 4] = g_disp[(SDK + 0x4c - g_code_lo) / 4] = ran;
    g_disp[(LAST + 0x48 - g_code_lo) / 4] = g_disp[(LAST + 0x4c - g_code_lo) / 4] = ran;
    g_log[0] = 0;
}

/* the runtime links the game itself */
static void switched(void) {
    GuestFn* slot = &g_disp[(START - g_code_lo) / 4];
    RtAPI rt = {.cas128 = snx_runtime_cas128};
    CPU c = {0};
    u64 at = 0, args[4] = {0};
    g_module.nmods = 3;
    game();
    g_exported = g_routed = g_ro_routed = 1;
    linker_decide();
    check(linker_links() && strstr(g_log, "the runtime linked the game's 3 modules itself") &&
              strstr(g_log, "2 relative relocations, 2 with a symbol, 1 PLT slots, each bound") &&
              strstr(g_log, "of 2 words that are weak (0) and of 0 that are not"),
          "the runtime links the game, and says what it linked");
    check(snx_ld_le64(GP(SDK) + DATA) == START && snx_ld_le64(GP(SDK) + DATA + 8) == SDK + 0x123 &&
              !snx_ld_le64(GP(SDK) + DATA + 0x10) && snx_ld_le64(GP(LAST) + DATA) == START,
          "the game's memory is relocated in place, a name two modules define bound to the first's");
    check(snx_ld_le64(GP(RTLD) + DATA) == START, "a PLT slot is bound to its function, not left lazy");
    linker_init();
    check(*slot == original, "nn::init::Start's slot is its own");
    check(linker_entry(0x999) == START, "the main thread enters nn::init::Start");
    check(linker_main(args) && args[0] == RTLD + MODULE_BYTES && args[1] == RTLD && args[2] == RTLD + 4 &&
              args[3] == RTLD + 8,
          "with the address behind the rtld and the runtime's three callbacks after its handle");
    check(g_disp[0] && g_disp[1] && g_disp[2], "whose dispatch slots hold the runtime's handlers");
    c.rt = &rt;
    c.mem = g_mem;
    c.disp = g_disp;
    c.code_lo = g_code_lo;
    c.code_hi = g_code_hi;
    c.x[30] = 0x5554;
    g_disp[0](&c, RTLD);
    check(c.pc == 0x5554 && c.x[30] == 0x5554 && !g_ran, "the first callback returns to its caller");
    c.pc = 0;
    g_disp[1](&c, RTLD + 4);
    check(g_ran == 2 && g_order[0] == LAST + 0x48 && g_order[1] == SDK + 0x48 && c.pc == 0x5554 && c.x[30] == 0x5554,
          "the second runs each module's DT_INIT but the rtld's, the last module first, and returns");
    c.pc = 0;
    g_disp[2](&c, RTLD + 8);
    check(g_ran == 4 && g_order[2] == SDK + 0x4c && g_order[3] == LAST + 0x4c && c.pc == 0x5554,
          "the third runs each DT_FINI in load order, but the rtld's");
    check(linker_find(NAME, &at) == 1 && at == START && !linker_find("nothing", &at),
          "a name is found where the modules bind it");
    check(linker_find_after(RTLD + 8, NAME, &at) == 1 && at == START, "past the rtld, at the SDK's definition");
    check(linker_find_after(SDK + MODULE_BYTES - 4, NAME, &at) == 1 && at == LAST + 0x44,
          "past the SDK, at the next module's, for a caller in the SDK's last word");
    at = 0;
    check(!linker_find_after(LAST + 4, NAME, &at) && !linker_find_after(SDK + MODULE_BYTES, NAME, &at) &&
              !linker_find_after(RTLD - 4, NAME, &at) && !at,
          "nowhere past the last module that defines it, nor for a caller no module holds");
}

int main(int argc, char** argv) {
    int differ = argc > 1 && !strcmp(argv[1], "differ");
    SnxRelocModule place[2] = {{"rtld", GP(RTLD), RTLD, MODULE_BYTES, GP(RTLD) + DATA, DATA, DATA_SIZE, 0, 0, 0},
                               {"sdk", GP(SDK), SDK, MODULE_BYTES, GP(SDK) + DATA, DATA, DATA_SIZE, 0, 0, 0}};
    SnxRelocCounts counts;
    GuestFn* slot = &g_disp[(START - g_code_lo) / 4];
    char error[256];
    CPU c = {0};
    if (argc > 1 && !strcmp(argv[1], "switch")) {
        switched();
        printf("runtime linker: %s\n", failures ? "FAILED" : "passed");
        return failures != 0;
    }
    /* where the game's linker cannot be watched, the slot is left alone and the log says why */
    game();
    g_exported = g_routed = g_ro_routed = g_nce = 1;
    linker_decide();
    linker_init();
    check(!linker_links() && !linker_main((u64[4]){0}) && *slot == original &&
              strstr(g_log, "links it: the game runs its own code (NCE)"),
          "under NCE the game's linker runs unwatched, and said");
    game();
    g_nce = 0, g_exported = 0;
    linker_decide();
    linker_init();
    check(!linker_links() && *slot == original && strstr(g_log, "no recompiled nn::init::Start") && strstr(g_log, NAME),
          "a game without nn::init::Start is said, with the names looked for");
    game();
    g_exported = 1, g_routed = 0, g_ro_routed = 0;
    linker_decide();
    linker_init();
    check(!linker_links() && *slot == original && strstr(g_log, "does not route nn::ro") &&
              strstr(g_log, "does not route " NAME) && strstr(g_log, "Recompile"),
          "a module that routes neither is said, with the symbol");
    /* each of these alone leaves the game to its own linker */
    game();
    g_routed = g_ro_routed = 1, g_family_off = 1;
    linker_decide();
    check(!linker_links() && strstr(g_log, "the nn_ro hooks are off"), "with nn::ro's hooks off the game links itself");
    game();
    g_family_off = 0, g_nros = (const SnxNros*)g_log;
    linker_decide();
    check(!linker_links() && strstr(g_log, "maps NROs through nn::ro"), "a game that maps NROs links itself");
    g_nros = NULL;
    /* watched: the slot is taken until its first call */
    game();
    g_ro_routed = 0;
    linker_decide();
    linker_init();
    check(*slot != original && strstr(g_log, "links it: the module does not route nn::ro") &&
              !strstr(g_log, "compared"),
          "nn::init::Start's slot is taken, and only who links is said yet");
    g_log[0] = 0;
    check(snx_ld_le64(GP(SDK) + DATA + 8) == 0x123 && snx_ld_le64(GP(RTLD) + DATA) == 0x700,
          "the game's memory is as it was loaded");
    /* the game's own linker: every word relocated, its slot of nn::init::Start left lazy */
    check(!snx_relocate(place, 2, &counts, error, sizeof error), "the game's linker relocates the modules");
    snx_st_le64(GP(RTLD) + DATA, RTLD + 0x700);
    if (differ) snx_st_le64(GP(SDK) + DATA, START + 4);
    (*slot)(&c, START);
    check(g_started == 1 && *slot == original, "the first call runs the function and gives its slot back");
    if (differ)
        check(strstr(g_log, "linker: 1 of 4 relocated words differ") && strstr(g_log, "the first in sdk at 20800") &&
                  strstr(g_log, "left 20044 and the runtime's 20040"),
              "a word that differs is said: how many, where, and both values");
    else
        check(strstr(g_log, "agree on 4 relocated words of 2 modules (1 relative, 1 with a symbol, 1 PLT slots, 1 of "
                            "them not bound yet)") &&
                  strstr(g_log, "nothing defines the symbol of 1 words that are weak (0) and of 0 that are not"),
              "what the two linkers agree on is said, by kind");
    original(&c, START);
    check(g_started == 2, "a later call is the function's alone");
    printf("runtime linker: %s\n", failures ? "FAILED" : "passed");
    return failures != 0;
}
