/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The runtime's side of the AOT pack: the variant table is looked up by the current
   guest bytes, a hit fills the dispatch slot, a changed word is refused, and
   invalidation clears the slot. A miss falls through to the JIT: the fake service
   here proves the runtime asks it only when no variant matches. */
#include "common/cpu/recompiler/llvm/jit_service.h"
#include "common/cpu/recompiler/variants.h"
#include "nx/loader/module_abi.h"
#include "nx/memory/mem.h"
#include "nx/runtime/guest_jit.h"
#include "nx/runtime/loader.h"
#include "nx/runtime/main.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"
#include "common/platform/dl.h"
#include "common/utilities/common.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CODE_LO 0x1000
#define CODE_HI 0x3000
#define START   0x2000
#define JIT_PC  0x2800
#define OUT     0x3000
#define OUT_SIZE 0x800

static int failures;
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                                    \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)

static u8 g_memory[0x4000];
u8* g_mem = g_memory;

static MemBlock g_block;
static SnxModInfo g_mods[1];
static SnxModule g_module;
const SnxModule* g_mod = &g_module;

static GuestFn g_dispatch[(CODE_HI - CODE_LO) / 4];
GuestFn* g_disp = g_dispatch;
u64 g_code_lo = CODE_LO, g_code_hi = CODE_HI;
const SnxNros* g_nros; /* main.c's: set by the NRO module checks */
int g_ounce; /* main.c's: a Switch program */

static SnxVariantRun g_run, g_run2;
static SnxU64 g_leader, g_leader2;
static SnxVariantFunction g_function, g_function2;
static SnxVariantFunction g_functions[2];
static SnxVariants g_variants_table;
const SnxVariants* g_variants = &g_variants_table;

char g_cachedir[1024];
char g_exename[256] = "runtime_variants";
char g_exedir[1024]; /* where the NRO module checks put their modules */

static int g_called;
static void test_fn(CPU* c, u64 entry) {
    (void)c;
    (void)entry;
    ++g_called;
}

static int g_called2;
static void test_fn2(CPU* c, u64 entry) {
    (void)c;
    (void)entry;
    ++g_called2;
}

/* ---- the JIT as much of it as guest_jit.c uses ---- */

static int g_jit_started, g_jit_added, g_jit_removed, g_jit_invalidated, g_jit_called;
static u64 g_jit_fallback_pc;
static char g_jit_opt;      /* the level jit_init gave the JIT */
static char g_warned[1024]; /* every WARN line, for the jit_opt checks */

static void test_jit_fn(CPU* c, u64 entry) {
    (void)c;
    (void)entry;
    ++g_jit_called;
}

/* only the pc under test is compiled, so the existing AOT checks stay honest */
static int jit_can_code(u64 pc) {
    if (g_jit_fallback_pc && pc == g_jit_fallback_pc) {
        if (pc >= OUT && pc < OUT + OUT_SIZE) return g_block.state == MS_CODE && (g_block.perm & PERM_X);
        return 1;
    }
    return pc == JIT_PC;
}

SnxJitService* snx_jit_service_new(const SnxJitConfig* config, char* error, size_t error_size) {
    static int sentinel;
    g_jit_opt = config->opt;
    if (error && error_size) error[0] = 0;
    g_jit_started = 1;
    return (SnxJitService*)(void*)&sentinel;
}

GuestFn snx_jit_resolve(SnxJitService* service, SnxU64 pc) {
    (void)service;
    return jit_can_code(pc) ? test_jit_fn : NULL;
}

GuestFn snx_jit_wait(SnxJitService* service, SnxU64 pc) { return snx_jit_resolve(service, pc); }

int snx_jit_covers(const SnxJitService* service, SnxU64 pc) {
    (void)service;
    (void)pc;
    return 0;
}

int snx_jit_add_image(SnxJitService* service, const SnxCodeImage* image) {
    (void)service;
    (void)image;
    ++g_jit_added;
    return 0;
}

void snx_jit_remove_image(SnxJitService* service, SnxU64 base) {
    (void)service;
    (void)base;
    ++g_jit_removed;
}

void snx_jit_invalidate(SnxJitService* service, SnxU64 addr, SnxU64 size) {
    (void)service;
    (void)addr;
    (void)size;
    ++g_jit_invalidated;
}

size_t snx_jit_warm(SnxJitService* service, const SnxU64* pcs, size_t count) {
    (void)service;
    (void)pcs;
    (void)count;
    return 0;
}

void snx_jit_stats(SnxJitService* service, SnxJitStats* stats) {
    (void)service;
    memset(stats, 0, sizeof(*stats));
    stats->threads = 1;
}

void snx_jit_service_free(SnxJitService* service) { (void)service; }

u64* hints_copy(size_t* count) {
    *count = 0;
    return NULL;
}

int nvn_loader_active(void) { return 0; }
/* nvn_loader.c's and hook.c's answers of what they put in a slot in the game's place: g_nvn_fn is NVN's one
   implementation here, and g_hook_fn the hook's handler of the function at g_hook_addr */
static GuestFn g_nvn_fn, g_hook_fn;
static u64 g_hook_addr;
int nvn_loader_handler(GuestFn fn) { return fn && fn == g_nvn_fn; }
GuestFn hook_function(u64 addr) { return g_hook_addr && addr == g_hook_addr ? g_hook_fn : NULL; }
/* the runtime's function in the game's place, twice (a hook's handler, NVN's implementation): each counts its calls
   as the function at 0x2400 */
static int g_override_called[2];
static void override_fn(CPU* c, u64 entry) {
    (void)c;
    g_override_called[0] += entry == 0x2400 ? 1 : 100;
}
static void override_fn2(CPU* c, u64 entry) {
    (void)c;
    g_override_called[1] += entry == 0x2400 ? 1 : 100;
}
/* no hooks here: nothing goes back in a slot */
void hook_restore(u64 addr, u64 size) {
    (void)addr;
    (void)size;
}

static int word(void* user, SnxU64 address, SnxU32* out) {
    (void)user;
    if (address > sizeof(g_memory) - 4 || (address & 3)) return -1;
    memcpy(out, g_memory + address, sizeof(*out));
    return 0;
}

void mem_query(u64 addr, MemBlock* out) {
    (void)addr;
    *out = g_block;
}

static void (*g_invalidate)(u64, u64);
void mem_set_code_invalidate(void (*fn)(u64, u64)) { g_invalidate = fn; }

static char g_logged[512]; /* the last LOG line */
void logf_(int lvl, const char* fmt, ...) {
    size_t at = strlen(g_warned);
    va_list ap;
    va_start(ap, fmt);
    if (lvl == 1) vsnprintf(g_logged, sizeof g_logged, fmt, ap);
    if (lvl == 0) vsnprintf(g_warned + at, sizeof g_warned - at, fmt, ap); /* WARN's level */
    va_end(ap);
}

/* the level the JIT got for jit_opt: a level LLVM takes as it is and nothing said, anything else said with the value
   given and the level it compiles at instead, settings.txt's default */
static void check_jit_opt(const char* given, char default_level) {
    char said[256];
    int before = failures;
    if (given[0] && !given[1] && strchr("0123sz", given[0])) {
        CHECK(g_jit_opt == given[0]);
        CHECK(!strstr(g_warned, "jit_opt"));
    } else {
        snprintf(said, sizeof said, "jit: jit_opt=%s is not 0, 1, 2, 3, s or z: it compiles at O%c\n", given,
                 default_level);
        CHECK(g_jit_opt == default_level);
        CHECK(strstr(g_warned, said) != NULL);
    }
    if (failures != before)
        fprintf(stderr, "jit_opt=%s gave the JIT O%c; the WARN lines: \"%s\"\n", given, g_jit_opt ? g_jit_opt : '?',
                g_warned);
}

/* An NRO's own AOT module (nro_module_fixture.c, at path), loaded when the NRO is mapped at the address g_nros gives
   it: found by the hash's name in <exedir><exename>_nro, checked to be that NRO's for that address, its entries put
   in the dispatch table and taken out again, its functions made stale by a change of their code, and each mapping
   starting from the module as it was made. A module of another ABI or another address is refused, said once */
typedef struct {
    u32 abi;
    u64 base;
    u32 imports;
} Fixture;
static int g_import_missed;
static void import_miss(CPU* c, u64 pc) {
    (void)c;
    g_import_missed += pc == 0x2500 ? 1 : 100;
}
/* The module's two imports, the game's functions at 0x2400 and 0x2500, each called once. What the first calls: 'G'
   the game module's function (test_fn2), 'H' and 'N' the runtime's two functions in its place, 'T' the dispatch table
   (which has test_fn there, or nothing: a miss), '?' anything else. The second has no function anywhere and must miss
   once, at 0x2500, through the table: else '!' */
static char imports_call(void* library) {
    RtAPI rt = {.cas128 = snx_runtime_cas128, .miss = import_miss};
    const SnxNroImports* (*get)(void);
    const SnxNroImports* im;
    GuestFn f[2];
    void* sym = plat_dl_sym(library, "snx_imports");
    CPU cpu = {0};
    int game = g_called2, table = g_called, hook = g_override_called[0], nvn = g_override_called[1];
    if (!sym) return '-';
    memcpy(&get, &sym, sizeof get);
    im = get();
    memcpy(f, im->function, sizeof f);
    cpu.rt = &rt;
    cpu.disp = g_disp;
    cpu.code_lo = g_code_lo;
    cpu.code_hi = g_code_hi;
    g_import_missed = 0;
    f[0](&cpu, 0x2400);
    f[1](&cpu, 0x2500);
    game = g_called2 - game, table = g_called - table + g_import_missed / 100;
    hook = g_override_called[0] - hook, nvn = g_override_called[1] - nvn;
    if (g_import_missed % 100 != 1 || cpu.x[1] == 0xD1) return '!';
    if (game + table + hook + nvn != 1) return '?';
    if (game) return f[0] == test_fn2 ? 'G' : '?';
    if (hook) return f[0] == override_fn ? 'H' : '?';
    if (nvn) return f[0] == override_fn2 ? 'N' : '?';
    return 'T';
}
static void nro_modules(const char* path) {
    static SnxNroSlot slots[3];
    static SnxNros table = {SNX_NROS_VERSION, 3, CODE_LO, CODE_HI, slots};
    static const u8 unknown[32] = {0x44};
    u8 id[32], other[32];
    char *root = snx_temp_dir(NULL), *folder, *file[3], name[80];
    void* own;
    Fixture* (*fixture)(void);
    const SnxModule* (*module)(void);
    const SnxModule* m;
    void* sym;
    CPU cpu = {0};
    u32 entries = 9;
    int k;
    CHECK(root != NULL);
    memset(id, 0x42, sizeof id);
    memset(other, 0x24, sizeof other);
    memset(slots, 0, sizeof slots);
    memset(slots[0].hash, 0x11, 32), slots[0].base = 0x1000, slots[0].size = 0x1000;
    memset(slots[1].hash, 0x22, 32), slots[1].base = 0x2000, slots[1].size = 0x1000;
    memset(slots[2].hash, 0x33, 32), slots[2].base = 0x2000, slots[2].size = 0x1000;
    snprintf(g_exedir, sizeof g_exedir, "%s/", root);
    folder = snx_format("%s/%s" SNX_NROS_FOLDER, root, g_exename);
    CHECK(!snx_mkdirs(folder));
    for (k = 0; k < 3; k++) {
        memset(name, "123"[k], 64);
        strcpy(name + 64, ".so");
        file[k] = snx_path_join(folder, name);
    }
    memset(g_disp, 0, sizeof g_dispatch);
    /* no table, an NRO the table has not, one mapped elsewhere, one of another size, one without a module file */
    CHECK(jit_nro_added(slots[0].hash, 0x1000, 0x1000, id, &entries) == 1 && !entries);
    g_nros = &table;
    CHECK(jit_nro_added(unknown, 0x1000, 0x1000, id, &entries) == 1);
    CHECK(jit_nro_added(slots[0].hash, 0x2000, 0x1000, id, &entries) == 1);
    CHECK(jit_nro_added(slots[0].hash, 0x1000, 0x2000, id, &entries) == 1);
    CHECK(jit_nro_added(slots[0].hash, 0x1000, 0x1000, id, &entries) == 1 && !entries);
    CHECK(!g_disp[(0x1100 - CODE_LO) / 4]);
    /* the game module's function at 0x2400, in its entries and its slot: what the NRO module's import of it calls */
    static const u64 game_addr[1] = {0x2400};
    static GuestFn game_fn[1] = {test_fn2};
    g_module.nentries = 1;
    g_module.entry_addr = game_addr;
    g_module.entry_fn = game_fn;
    g_disp[(0x2400 - CODE_LO) / 4] = test_fn2;
    /* its module: both entries in the table, each its function */
    CHECK(!snx_copy_file(path, file[0]));
    own = plat_dl_open(file[0]);
    CHECK(own != NULL);
    sym = plat_dl_sym(own, "snx_module");
    CHECK(sym != NULL);
    memcpy(&module, &sym, sizeof module);
    m = module();
    g_warned[0] = 0;
    CHECK(jit_nro_added(slots[0].hash, 0x1000, 0x1000, id, &entries) == 0 && entries == 2);
    CHECK(g_disp[(0x1100 - CODE_LO) / 4] == m->fn[0] && g_disp[(0x1200 - CODE_LO) / 4] == m->fn[1]);
    if (g_disp[(0x1200 - CODE_LO) / 4]) g_disp[(0x1200 - CODE_LO) / 4](&cpu, 0x1200);
    CHECK(cpu.x[0] == 0x1200 && !g_warned[0]);
    /* its imports: the game module has a function at 0x2400 and its slot holds it, so the import calls it directly;
       it has none at 0x2500, which goes through the table (one miss, at 0x2500) */
    CHECK(strstr(g_logged, "the NRO at 1000 calls 1 of the 2 functions of the game its module imports directly, 0 of "
                           "them the runtime's own"));
    CHECK(imports_call(own) == 'G');
    /* a slot that holds a function nobody answers for (code the JIT made, which goes when the code under it changes)
       keeps the call in the table, whatever a hook has for another address */
    g_hook_addr = 0x2404, g_hook_fn = test_fn;
    g_disp[(0x2400 - CODE_LO) / 4] = test_fn;
    jit_nro_removed(0x1000);
    CHECK(jit_nro_added(slots[0].hash, 0x1000, 0x1000, id, &entries) == 0);
    CHECK(strstr(g_logged, "calls 0 of the 2 functions") && imports_call(own) == 'T');
    /* the slot holds a hook's handler for that address: bound to the handler, in the game's function's place, and
       called without the table (the slot emptied after) */
    g_hook_addr = 0x2400, g_hook_fn = override_fn;
    g_disp[(0x2400 - CODE_LO) / 4] = override_fn;
    jit_nro_removed(0x1000);
    CHECK(jit_nro_added(slots[0].hash, 0x1000, 0x1000, id, &entries) == 0);
    CHECK(strstr(g_logged, "calls 1 of the 2 functions of the game its module imports directly, 1 of them the "
                           "runtime's own in the game's place (hooks, NVN)"));
    g_disp[(0x2400 - CODE_LO) / 4] = NULL;
    CHECK(imports_call(own) == 'H');
    /* the hook's handler is another function than the slot holds: the table */
    g_hook_fn = override_fn2;
    g_disp[(0x2400 - CODE_LO) / 4] = override_fn;
    jit_nro_removed(0x1000);
    CHECK(jit_nro_added(slots[0].hash, 0x1000, 0x1000, id, &entries) == 0);
    g_disp[(0x2400 - CODE_LO) / 4] = test_fn;
    CHECK(strstr(g_logged, "calls 0 of the 2 functions") && imports_call(own) == 'T');
    g_hook_addr = 0, g_hook_fn = NULL;
    /* the slot holds one of NVN's functions: bound to it */
    g_nvn_fn = override_fn2;
    g_disp[(0x2400 - CODE_LO) / 4] = override_fn2;
    jit_nro_removed(0x1000);
    CHECK(jit_nro_added(slots[0].hash, 0x1000, 0x1000, id, &entries) == 0);
    g_disp[(0x2400 - CODE_LO) / 4] = NULL;
    CHECK(strstr(g_logged, "calls 1 of the 2 functions") && imports_call(own) == 'N');
    g_nvn_fn = NULL;
    /* bound to the game's function, and the slot given the runtime's while the NRO is mapped (NVN's, for a function
       the game resolved then; a hook's): the import calls that function from then on, the override winning, and
       another address's taking changes nothing */
    g_disp[(0x2400 - CODE_LO) / 4] = test_fn2;
    jit_nro_removed(0x1000);
    CHECK(jit_nro_added(slots[0].hash, 0x1000, 0x1000, id, &entries) == 0 && entries == 2);
    CHECK(imports_call(own) == 'G');
    jit_slot_taken(0x2404, override_fn);
    jit_slot_taken(0x2300, override_fn);
    CHECK(imports_call(own) == 'G');
    jit_slot_taken(0x2400, override_fn2);
    CHECK(imports_call(own) == 'N');
    jit_slot_taken(0x2400, override_fn);
    CHECK(imports_call(own) == 'H');
    jit_nro_removed(0x1000);
    CHECK(jit_nro_added(slots[0].hash, 0x1000, 0x1000, id, &entries) == 0 && entries == 2);
    CHECK(imports_call(own) == 'G');
    /* the game changes a line of the first function: it is stale and out of the table, its entry too; the second
       stays */
    jit_code_changed(0x1120, 64);
    CHECK(m->stale[0] == 1 && !m->stale[1] && !m->entry_fn[0] && m->entry_fn[1] == m->fn[1]);
    CHECK(!g_disp[(0x1100 - CODE_LO) / 4] && g_disp[(0x1200 - CODE_LO) / 4] == m->fn[1]);
    /* unmapped: its entries leave the table, and a change of that memory is no change of its code any more */
    jit_nro_removed(0x1000);
    CHECK(!g_disp[(0x1200 - CODE_LO) / 4]);
    jit_code_changed(0x1200, 64);
    CHECK(!m->stale[1]);
    /* mapped again: the module as it was made, but a slot something else holds by then keeps what it holds */
    g_disp[(0x1200 - CODE_LO) / 4] = test_fn;
    CHECK(jit_nro_added(slots[0].hash, 0x1000, 0x1000, id, &entries) == 0 && entries == 1);
    CHECK(!m->stale[0] && m->entry_fn[0] == m->fn[0] && g_disp[(0x1100 - CODE_LO) / 4] == m->fn[0]);
    CHECK(g_disp[(0x1200 - CODE_LO) / 4] == test_fn);
    jit_nro_removed(0x1000);
    CHECK(!g_disp[(0x1100 - CODE_LO) / 4] && g_disp[(0x1200 - CODE_LO) / 4] == test_fn);
    g_disp[(0x1200 - CODE_LO) / 4] = NULL;
    /* the module of another NRO (its module ID is not the mapped one's): refused, and not asked again */
    g_warned[0] = 0;
    CHECK(jit_nro_added(slots[0].hash, 0x1000, 0x1000, other, &entries) == -1 && !entries);
    CHECK(strstr(g_warned, "the NRO at 1000 has an AOT module made from another NRO (module ID 42424242..., where the "
                           "mapped one's is 24242424...)") &&
          !g_disp[(0x1100 - CODE_LO) / 4]);
    g_warned[0] = 0;
    CHECK(jit_nro_added(slots[0].hash, 0x1000, 0x1000, id, &entries) == 1 && !g_warned[0]);
    plat_dl_close(own);
    /* a module of another ABI, and one made for another address (0x1000, where its NRO is at 0x2000) */
    CHECK(!snx_copy_file(path, file[1]) && !snx_copy_file(path, file[2]));
    own = plat_dl_open(file[1]);
    CHECK(own != NULL);
    sym = plat_dl_sym(own, "snx_fixture");
    CHECK(sym != NULL);
    memcpy(&fixture, &sym, sizeof fixture);
    fixture()->abi = SNX_ABI + 1;
    fixture()->base = 0x2000;
    g_warned[0] = 0;
    CHECK(jit_nro_added(slots[1].hash, 0x2000, 0x1000, id, &entries) == -1);
    snprintf(name, sizeof name, "of ABI %u, where this runtime runs %u", (unsigned)SNX_ABI + 1, (unsigned)SNX_ABI);
    CHECK(strstr(g_warned, name) && strstr(g_warned, "the NRO at 2000 "));
    plat_dl_close(own);
    g_warned[0] = 0;
    CHECK(jit_nro_added(slots[2].hash, 0x2000, 0x1000, id, &entries) == -1);
    CHECK(strstr(g_warned, "the NRO at 2000 (4096 bytes) has an AOT module made for 1000 (4096 bytes, 1 modules)"));
    CHECK(!g_disp[(0x2100 - CODE_LO) / 4] && !g_disp[(0x1100 - CODE_LO) / 4]);
    g_nros = NULL;
    for (k = 0; k < 3; k++) free(file[k]);
    free(folder);
    CHECK(!snx_remove_tree(root));
    free(root);
}

int main(int argc, char** argv) {
    static const SnxU32 words[] = {0x91000400u, 0xD65F03C0u};  /* add x0, x0, #1; ret */
    static const SnxU32 words2[] = {0x91000800u, 0xD65F03C0u}; /* add x0, x0, #2; ret */
    /* --jit-opt=V: jit_opt as the .ini gives it. 3 by default, neither settings.txt's default nor a phone's O0 */
    const char* jit_opt = "3";
    const char default_level = g_settings.jit_optimization[0]; /* settings.txt's, before any .ini */
    GuestFn fn;
    for (int i = 1; i < argc; i++)
        if (!strncmp(argv[i], "--jit-opt=", 10)) jit_opt = argv[i] + 10;
    memcpy(g_memory + START, words, sizeof(words));
    memcpy(g_memory + OUT, words2, sizeof(words2));
    memset(&g_block, 0, sizeof(g_block));
    g_block.addr = CODE_LO;
    g_block.size = OUT + OUT_SIZE - CODE_LO;
    g_block.state = MS_CODE;
    g_block.perm = PERM_RX;
    memset(g_mods, 0, sizeof(g_mods));
    g_mods[0].name = "test";
    g_mods[0].base = CODE_LO;
    g_mods[0].size = CODE_HI - CODE_LO;
    g_mods[0].text_off = 0;
    g_mods[0].text_size = CODE_HI - CODE_LO;
    memset(&g_module, 0, sizeof(g_module));
    g_module.abi = SNX_ABI;
    g_module.nmods = 1;
    g_module.mods = g_mods;
    g_module.code_lo = CODE_LO;
    g_module.code_hi = CODE_HI;
    g_module.title = "test";
    g_leader = START;
    g_run.address = START;
    g_run.words = 2;
    g_function.start = START;
    g_function.leader_count = 1;
    g_function.leaders = &g_leader;
    g_function.run_count = 1;
    g_function.runs = &g_run;
    g_function.fn = test_fn;
    CHECK(snx_variant_hash(word, NULL, &g_run, 1, &g_function.h1, &g_function.h2) == 0);
    g_leader2 = OUT;
    g_run2.address = OUT;
    g_run2.words = 2;
    g_function2.start = OUT;
    g_function2.leader_count = 1;
    g_function2.leaders = &g_leader2;
    g_function2.run_count = 1;
    g_function2.runs = &g_run2;
    g_function2.fn = test_fn2;
    CHECK(snx_variant_hash(word, NULL, &g_run2, 1, &g_function2.h1, &g_function2.h2) == 0);
    /* the table's order is the build's; the runtime index sorts it */
    g_functions[0] = g_function2;
    g_functions[1] = g_function;
    g_variants_table.version = SNX_VARIANTS_VERSION;
    g_variants_table.count = 2;
    g_variants_table.functions = g_functions;
    snprintf(g_settings.jit_optimization, sizeof g_settings.jit_optimization, "%s", jit_opt);
    g_settings.jit_threads = 1;

    jit_init();
    CHECK(g_jit_started == 1);
    check_jit_opt(jit_opt, default_level);
    if (argc == 2 && !strcmp(argv[1], "--bench-no-pack")) {
        g_variants = NULL;
        for (u64 i = 0; i < 20000000; i++)
            if (jit_resolve(JIT_PC) != test_jit_fn) return 1;
        puts("runtime_variants: no-pack replay ok");
        return 0;
    }
    CHECK(g_invalidate == jit_code_invalidate);
    CHECK(g_disp[(START - CODE_LO) / 4] == NULL);
    fn = jit_wait(START);
    CHECK(fn == test_fn);
    if (fn) fn(NULL, START);
    CHECK(g_called == 1);
    CHECK(g_disp[(START - CODE_LO) / 4] == test_fn);
    CHECK(jit_resolve(START) == test_fn);
    CHECK(jit_wait(START) == test_fn);

    /* no variant for JIT_PC: the runtime asks the JIT and runs its function */
    fn = jit_wait(JIT_PC);
    CHECK(fn == test_jit_fn);
    if (fn) fn(NULL, JIT_PC);
    CHECK(g_jit_called == 1);
    CHECK(jit_resolve(JIT_PC) == test_jit_fn);

    /* a variant whose bytes changed is refused, and the JIT compiles the current bytes */
    g_jit_fallback_pc = START;
    g_memory[START] ^= 1;
    jit_code_invalidate(START, 4);
    CHECK(g_disp[(START - CODE_LO) / 4] == NULL);
    CHECK(jit_wait(START) == test_jit_fn);
    CHECK(jit_resolve(START) == test_jit_fn);

    g_memory[START] ^= 1;
    g_jit_fallback_pc = 0;
    jit_code_invalidate(START, 4);
    CHECK(jit_wait(START) == test_fn);
    /* a pc outside code_lo..hi is looked up through a block the game mapped */
    CHECK(jit_wait(OUT) == NULL);
    fn = jit_resolve_or_add(OUT);
    CHECK(fn == test_fn2);
    if (fn) fn(NULL, OUT);
    CHECK(g_called2 == 1);
    CHECK(jit_resolve(OUT) == test_fn2);
    /* the block stops being code: invalidation drops the synthetic image */
    g_block.state = MS_NORMAL;
    g_block.perm = PERM_RW;
    jit_code_invalidate(OUT, 4);
    CHECK(jit_wait(OUT) == NULL);
    CHECK(jit_resolve_or_add(OUT) == NULL);
    /* a pc outside the table, in the same block, has no variant */
    CHECK(jit_wait(OUT + 0x400) == NULL);
    CHECK(jit_resolve(OUT + 0x400) == NULL);
    CHECK(jit_resolve_or_add(OUT + 0x400) == NULL);
    CHECK(g_jit_added == 1);
    CHECK(g_jit_removed == 1);
    CHECK(g_jit_invalidated > 0);
    const SnxVariants* pack = g_variants;
    g_variants = NULL;
    CHECK(jit_resolve(JIT_PC) == test_jit_fn);
    CHECK(jit_wait(JIT_PC) == test_jit_fn);
    g_variants = pack;
    jit_report();
    for (int i = 1; i < argc; i++)
        if (!strncmp(argv[i], "--nro-module=", 13)) nro_modules(argv[i] + 13);

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    puts("runtime_variants: ok");
    return 0;
}
