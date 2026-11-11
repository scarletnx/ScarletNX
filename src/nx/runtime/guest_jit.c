/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The runtime's side of the AOT pack and the JIT: a pc the game module has no static function for is looked up in
   the module's dense variant index first, then compiled by the runtime JIT when the pack has no matching code. The
   index is a runtime projection of the const variant table (variants.h); it is sorted once, so the miss path does not
   walk the table's pointer chains. There is no interpreter: when neither the pack nor the JIT has code, the game ends
   with the reason (rtapi.c's rt_miss) */
#include "nx/runtime/guest_jit.h"
#include "nx/runtime/loader.h"
#include "nx/runtime/main.h"
#include "nx/runtime/options.h"
#include "nx/runtime/rtapi.h"
#include "nx/gpu/nvn/nvn_loader.h"
#include "nx/hle/hook/hook.h"
#include "nx/memory/mem.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include "nx/runtime/platform/tracy.h"
#include "common/cpu/recompiler/image.h"
#include "common/cpu/recompiler/variants.h"
#include "common/cpu/recompiler/llvm/jit_service.h"
#include "common/platform/dl.h"
#include "common/utilities/common.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
/* an NRO's own AOT module is a library the system loads, which iOS loads none of but the app's: the JIT runs every
   NRO there (a pack of an NRO's objects, as the game's code is one, is not made) */
#if defined(__APPLE__) && TARGET_OS_IOS
#define NRO_MODULES 0
#else
#define NRO_MODULES 1
#endif

static SnxJitService* g_jit;
static char g_jit_error[512];      /* why there is no JIT, for the fatal error of the first miss */
static char g_jit_cache[1400];     /* <Game>.jitcache in the user's cache folder: the units of earlier runs */
static SnxCodeImage* g_jit_images; /* the modules' text in guest memory, for the variant hash and the JIT */
static SnxCodeImage g_jit_synthetic[SNX_JIT_IMAGES]; /* blocks the game maps, for the variant hash and the JIT */
static PMutex g_jit_synthetic_lock = PMUTEX_INIT;
static unsigned long g_jit_hits, g_jit_misses;

/* the hot lookup: one leader per entry, sorted by pc. The const table's function pointers and run pointers stay cold */
typedef struct {
    SnxU64 pc;
    SnxU32 function;
} VariantLeader;

static VariantLeader* g_variant_leaders;
static size_t g_variant_leader_count;

/* the text one image covers from the pc that missed: a block the game maps may be large, and the hash reads only the
   words the variant's runs name */
#define JIT_SYNTHETIC_MAX (4u << 20)

typedef struct {
    const SnxCodeImage* modules;
    size_t module_count;
    const SnxCodeImage* synthetic;
    size_t synthetic_count;
} VariantImages;

/* reads one guest word from a module's text or a block the game mapped */
static int image_word(void* user, SnxU64 address, SnxU32* word) {
    const VariantImages* images = (const VariantImages*)user;
    size_t i;
    if (address & 3) return -1;
    for (i = 0; i < images->module_count; ++i) {
        const SnxCodeImage* image = &images->modules[i];
        SnxU64 start = image->base + image->text_off;
        if (image->text_size >= 4 && address >= start && address - start <= image->text_size - 4) {
            memcpy(word, image->image + (address - image->base), sizeof(*word));
            return 0;
        }
    }
    for (i = 0; i < images->synthetic_count; ++i) {
        const SnxCodeImage* image = &images->synthetic[i];
        if (image->text_size >= 4 && address >= image->base && address - image->base <= image->text_size - 4) {
            memcpy(word, image->image + (address - image->base), sizeof(*word));
            return 0;
        }
    }
    return -1;
}

static int leader_compare(const void* left, const void* right) {
    const VariantLeader* a = (const VariantLeader*)left;
    const VariantLeader* b = (const VariantLeader*)right;
    if (a->pc < b->pc) return -1;
    if (a->pc > b->pc) return 1;
    if (a->function < b->function) return -1;
    if (a->function > b->function) return 1;
    return 0;
}

/* one dense index for the const table's leaders. Failure leaves the pack unused and the JIT handles every miss */
static int variant_index_build(void) {
    size_t count = 0, at = 0, i;
    u32 j;
    if (!g_variants || !g_variants->functions) return 0;
    for (i = 0; i < g_variants->count; ++i) {
        const SnxVariantFunction* function = &g_variants->functions[i];
        if (function->leader_count > ~(size_t)0 - count) return -1;
        count += function->leader_count;
    }
    if (!count) return 0;
    if (count > ~(size_t)0 / sizeof(*g_variant_leaders)) return -1;
    g_variant_leaders = (VariantLeader*)malloc(count * sizeof(*g_variant_leaders));
    if (!g_variant_leaders) return -1;
    for (i = 0; i < g_variants->count; ++i) {
        const SnxVariantFunction* function = &g_variants->functions[i];
        for (j = 0; j < function->leader_count; ++j) {
            g_variant_leaders[at].pc = function->leaders[j];
            g_variant_leaders[at].function = (SnxU32)i;
            ++at;
        }
    }
    qsort(g_variant_leaders, count, sizeof(*g_variant_leaders), leader_compare);
    for (i = 1; i < count; ++i) assert(g_variant_leaders[i - 1].pc <= g_variant_leaders[i].pc);
    g_variant_leader_count = count;
    LOG("jit: %zu AOT variant leaders indexed\n", count);
    return 0;
}

/* the first entry whose pc is not less than target */
static size_t variant_lower_bound(SnxU64 target) {
    size_t lo = 0, hi = g_variant_leader_count;
    while (lo < hi) {
        size_t middle = lo + (hi - lo) / 2;
        if (g_variant_leaders[middle].pc < target)
            lo = middle + 1;
        else
            hi = middle;
    }
    return lo;
}

/* the AOT function whose current bytes hash-match pc, or NULL */
static GuestFn variant_resolve(SnxU64 pc) {
    VariantImages images;
    SnxU64 h1, h2;
    GuestFn result = NULL;
    size_t i;
    /* Without a usable pack there is no lookup to count. A global atomic here serialized guest threads even when
       every call went straight to an already compiled JIT function. Hits and misses count actual pack lookups. */
    if (!g_variants || !g_jit_images || !g_variant_leaders) return NULL;
    images.modules = g_jit_images;
    images.module_count = g_mod->nmods;
    images.synthetic = g_jit_synthetic;
    images.synthetic_count = SNX_JIT_IMAGES;
    pm_lock(&g_jit_synthetic_lock);
    i = variant_lower_bound(pc);
    for (; i < g_variant_leader_count && g_variant_leaders[i].pc == pc; ++i) {
        const SnxVariantFunction* function = &g_variants->functions[g_variant_leaders[i].function];
        if (snx_variant_hash(image_word, &images, function->runs, function->run_count, &h1, &h2)) continue;
        if (h1 != function->h1 || h2 != function->h2) continue;
        if (pc >= g_code_lo && pc < g_code_hi) {
            GuestFn expected = NULL;
            __atomic_compare_exchange_n(&g_disp[(pc - g_code_lo) >> 2], &expected, function->fn, 0, __ATOMIC_RELEASE,
                                        __ATOMIC_RELAXED);
        }
        result = function->fn;
        break;
    }
    pm_unlock(&g_jit_synthetic_lock);
    if (result)
        __atomic_add_fetch(&g_jit_hits, 1, __ATOMIC_RELAXED);
    else
        __atomic_add_fetch(&g_jit_misses, 1, __ATOMIC_RELAXED);
    return result;
}

/* What a batch took and what the queues hold, on the timeline. A batch runs on a compile thread, whose name the
   profiler is given with its first batch, and the sums since that thread's last batch give the cache loads and the
   batches compiled. A batch's seconds are the LLVM pipeline's and the loader's, so a hitch in a run that compiles
   shows which of the two it was. */
static _Thread_local struct {
    int named;
    unsigned long cached, batches;
} t_jit_plot;

static void jit_report_plots(const SnxJitReport* r) {
    SnxJitStats stats;
    if (!snx_tracy_enter()) return; /* no client: a batch's numbers go nowhere */
    if (!t_jit_plot.named) {
        t_jit_plot.named = 1;
        SNX_THREAD_NAME("jit compile"); /* the system's name is set where the thread starts (jit_service.c) */
    }
    /* the service's sums: read under its lock, which this thread does not hold (the report comes outside them) */
    if (g_jit) {
        snx_jit_stats(g_jit, &stats);
        unsigned long cached = (unsigned long)stats.cached, batches = (unsigned long)stats.batches;
        [[maybe_unused]] unsigned long queued = (unsigned long)stats.queued; /* the client's plot only */
        SNX_PLOT("jit cache loads", (double)(cached - t_jit_plot.cached));
        SNX_PLOT("jit batches", (double)(batches - t_jit_plot.batches));
        t_jit_plot.cached = cached;
        t_jit_plot.batches = batches;
        SNX_PLOT("jit queue depth", (double)queued);
    }
    SNX_PLOT("jit compile ms", r->seconds * 1000.0);
    SNX_PLOT("jit batch size", (double)r->pcs);
    SNX_PLOT("jit batch functions", (double)r->functions);
    if (r->dropped)
        SNX_PLOT("jit dropped batches", 1);
    else if (r->error)
        SNX_PLOT("jit failures", 1);
    else
        SNX_PLOT("jit compiled pcs", (double)r->pcs);
    snx_tracy_leave();
}

/* on a compile thread */
static void jit_log(const SnxJitReport* r) {
    if (r->dropped)
        LOGV("jit: %llx and %lu more pcs queued again: guest memory changed while they compiled\n",
             (unsigned long long)r->pc, (unsigned long)(r->pcs - 1));
    else if (r->error)
        WARN_LIMIT(32, "jit: %llx and %lu more pcs could not be compiled: %s\n", (unsigned long long)r->pc,
                   (unsigned long)(r->pcs - 1), r->error);
    else
        LOGV("jit: %llx and %lu more pcs compiled, %lu functions, %lu instructions, %lu KiB, %.1f ms\n",
             (unsigned long long)r->pc, (unsigned long)(r->pcs - 1), (unsigned long)r->functions,
             (unsigned long)r->instructions, (unsigned long)(r->bytes >> 10), r->seconds * 1000.0);
    jit_report_plots(r);
}

/* on the thread it tells of, once */
static void jit_thread_log(const SnxJitThread* t) {
    char name[32];
    if (t->misses)
        snprintf(name, sizeof(name), "the miss thread");
    else
        snprintf(name, sizeof(name), "compile thread %u", t->index);
    if (!t->scale)
        LOG("jit: %s runs at the priority the process started it with: the JIT sets none on this host\n", name);
    else if (t->priority != t->asked)
        WARN("jit: %s runs at %s %d: the host refused %s %d\n", name, t->scale, t->priority, t->scale, t->asked);
    else
        LOG("jit: %s runs at %s %d%s\n", name, t->scale, t->priority,
            t->misses ? ": it compiles what a game thread waits for" : "");
}

void jit_init(void) {
    SnxJitConfig config;
    SnxJitStats stats;
    char error[512];
    char opt;
    u64* hints;
    size_t hint_count, warm = 0;
    u32 i;
    g_jit_images = (SnxCodeImage*)calloc(g_mod->nmods ? g_mod->nmods : 1, sizeof(*g_jit_images));
    if (!g_jit_images) {
        snprintf(g_jit_error, sizeof(g_jit_error), "out of memory");
        WARN("jit: out of memory, so only the code the recompiler found can run\n");
        return;
    }
    for (i = 0; i < g_mod->nmods; i++) {
        const SnxModInfo* m = &g_mod->mods[i];
        g_jit_images[i].image = GP(m->base);
        g_jit_images[i].image_size = (size_t)m->size;
        g_jit_images[i].base = m->base;
        g_jit_images[i].text_off = m->text_off;
        g_jit_images[i].text_size = m->text_size;
    }
    if (g_variants) {
        LOG("jit: %u runtime code variants in the AOT module\n", g_variants->count);
        if (variant_index_build())
            WARN("jit: out of memory, so the AOT pack cannot be looked up; the JIT handles every miss\n");
    } else {
        WARN("jit: the AOT module has no runtime code variants; the JIT handles the misses and writes a cache\n");
    }
    memset(&config, 0, sizeof(config));
#if defined(__ANDROID__) || defined(SNX_IOS)
    /* a phone compiles at O0: any other level breaks the JIT there, so settings.txt has no setting for it, and the ini
       has no key */
    opt = '0';
#else
    opt = g_settings.jit_optimization[0];
    /* ini_read takes any text that fits, as a hand-edited .ini has it; strchr finds an empty one's NUL */
    if (!opt || !strchr("0123sz", opt)) {
        opt = '2'; /* settings.txt's default, as a setting keeps its earlier value for one it does not take */
        WARN("jit: jit_opt=%s is not 0, 1, 2, 3, s or z: it compiles at O%c\n", g_settings.jit_optimization, opt);
    }
#endif
    /* an Ounce game's CPU, as the recompiler's module_llvm.c gives it from the same NPDM */
    config.features = g_settings.cpu_extensions || g_ounce ? SNX_A64_TARGET_SWITCH_EXTENDED : 0;
    config.opt = opt;
    config.images = g_jit_images;
    config.image_count = g_mod->nmods;
    config.dispatch = g_disp;
    config.code_lo = g_code_lo;
    config.code_hi = g_code_hi;
    config.aot_addresses = g_mod->entry_addr;
    config.aot_functions = g_mod->entry_fn;
    config.aot_count = g_mod->nentries;
    /* with NVN taken, its handlers sit in dispatch slots, which only calls through the table see */
    config.hook_direct_calls = nvn_loader_active();
    config.direct_svc = 1; /* g_rtapi's svcs, as the AOT module calls them */
    config.threads = (unsigned)g_settings.jit_threads;
    config.region_instructions = (size_t)g_settings.jit_region_size;
    config.live_budget = (size_t)g_settings.jit_live_budget;
    config.report = jit_log;
    config.thread = jit_thread_log;
    snprintf(g_jit_cache, sizeof(g_jit_cache), "%s%s.jitcache", g_cachedir, g_exename);
    config.cache_path = g_jit_cache;
    g_jit = snx_jit_service_new(&config, error, sizeof(error));
    if (!g_jit) {
        snprintf(g_jit_error, sizeof(g_jit_error), "%s", error);
        WARN("jit: cannot start (%s), so only the AOT pack can run\n", error);
    } else {
        /* the pack wins the pcs it covers; warming the hints would compile them again */
        if (!g_variants || !g_variants->count) {
            hints = hints_copy(&hint_count);
            if (hints) warm = snx_jit_warm(g_jit, hints, hint_count);
            free(hints);
        }
        snx_jit_stats(g_jit, &stats);
        LOG("jit: %u compile threads and the miss thread, %lu pcs of earlier runs queued\n", stats.threads,
            (unsigned long)warm);
    }
    /* mapping, permission and write-back changes drop the slots and images the range's code filled (mem.c) */
    mem_set_code_invalidate(jit_code_invalidate);
}

GuestFn jit_resolve(u64 pc) {
    GuestFn code;
    if (pc >= g_code_lo && pc < g_code_hi) {
        code = __atomic_load_n(&g_disp[(pc - g_code_lo) >> 2], __ATOMIC_ACQUIRE);
        if (code) return code;
    }
    code = variant_resolve(pc);
    if (code) return code;
    return g_jit ? snx_jit_resolve(g_jit, pc) : NULL;
}

GuestFn jit_wait(u64 pc) {
    GuestFn code = variant_resolve(pc);
    [[maybe_unused]] u64 t0 = 0;
    int trace;
    /* the wait a miss costs the game: one a compile thread is working on waits on its condition, and one it has not
       taken yet is queued for it. Only the wait is measured: the resolve above is a table read. Its zone is left out:
       the wait can span a guest thread's own block (rt_miss's kthread_block), which leaves a zone open for good */
    if (code) return code;
    if ((trace = snx_tracy_enter())) t0 = plat_ns();
    code = g_jit ? snx_jit_wait(g_jit, pc) : NULL;
    if (trace) { /* the gate is open, so the client is up: the wait is measured */
        SNX_PLOT("jit miss ms", (double)(plat_ns() - t0) / 1e6);
        snx_tracy_leave();
    }
    return code;
}

/* whether pc is in executable guest memory, and the block it is in */
static int code_at(u64 pc, MemBlock* block) {
    if (pc & 3) return 0;
    mem_query(pc, block);
    return block->state != MS_FREE && (block->perm & PERM_X) && block->size >= 4 && pc >= block->addr &&
           pc - block->addr < block->size;
}

/* whether a module image or a block the game mapped already covers pc */
static int image_covers(u64 pc) {
    size_t i;
    if (!g_jit_images) return 0;
    for (i = 0; i < g_mod->nmods; ++i) {
        const SnxCodeImage* image = &g_jit_images[i];
        SnxU64 start = image->base + image->text_off;
        if (image->text_size && pc >= start && pc - start < image->text_size) return 1;
    }
    for (i = 0; i < SNX_JIT_IMAGES; ++i) {
        const SnxCodeImage* image = &g_jit_synthetic[i];
        if (image->text_size && pc >= image->base && pc - image->base < image->text_size) return 1;
    }
    return 0;
}

/* the image for the block pc is in, unless one already covers it; 1 when resolve can look pc up */
static int jit_add_synthetic(u64 pc) {
    MemBlock block;
    SnxCodeImage image;
    u64 text;
    size_t i, slot = SNX_JIT_IMAGES;
    if (!g_jit_images) return 0;
    if (!code_at(pc, &block)) return 0;
    pm_lock(&g_jit_synthetic_lock);
    if (image_covers(pc)) {
        pm_unlock(&g_jit_synthetic_lock);
        return 1;
    }
    for (i = 0; i < SNX_JIT_IMAGES; ++i) {
        if (!g_jit_synthetic[i].text_size) {
            slot = i;
            break;
        }
    }
    if (slot == SNX_JIT_IMAGES) {
        pm_unlock(&g_jit_synthetic_lock);
        return 0; /* every image slot is a block the game mapped: this pc can't be looked up */
    }
    text = block.addr + block.size - pc;
    if (text > JIT_SYNTHETIC_MAX) text = JIT_SYNTHETIC_MAX;
    memset(&image, 0, sizeof(image));
    image.image = GP(pc);
    image.image_size = (size_t)text;
    image.base = pc;
    image.text_size = text;
    if (g_jit && snx_jit_add_image(g_jit, &image)) {
        pm_unlock(&g_jit_synthetic_lock);
        return 0;
    }
    g_jit_synthetic[slot] = image;
    pm_unlock(&g_jit_synthetic_lock);
    return 1;
}

GuestFn jit_resolve_or_add(u64 pc) {
    GuestFn code;
    if (!g_jit && !g_variants) return NULL;
    code = jit_resolve(pc);
    if (code) return code;
    if (!jit_add_synthetic(pc)) return NULL;
    return jit_resolve(pc);
}

const char* jit_failure(void) {
    if (g_jit) return "the AOT pack has no code for it, and the JIT could not compile it";
    if (g_variants) return "the AOT pack has no code for it, and the JIT is not available";
    return g_jit_error[0] ? g_jit_error : "no AOT pack and no JIT";
}

int jit_code_added(u64 base, u64 text_size, u64 size) {
    SnxCodeImage image;
    size_t i, slot = SNX_JIT_IMAGES;
    if (!g_jit_images) return -1;
    if (!g_jit && !g_variants) return -1;
    memset(&image, 0, sizeof(image));
    image.image = GP(base);
    image.image_size = (size_t)size;
    image.base = base;
    image.text_size = text_size;
    pm_lock(&g_jit_synthetic_lock);
    for (i = 0; i < SNX_JIT_IMAGES; ++i) {
        if (!g_jit_synthetic[i].text_size) {
            slot = i;
            break;
        }
    }
    if (slot == SNX_JIT_IMAGES) {
        pm_unlock(&g_jit_synthetic_lock);
        return -1;
    }
    if (g_jit && snx_jit_add_image(g_jit, &image)) {
        pm_unlock(&g_jit_synthetic_lock);
        return -1;
    }
    g_jit_synthetic[slot] = image;
    pm_unlock(&g_jit_synthetic_lock);
    return 0;
}

void jit_code_removed(u64 base) {
    size_t i;
    if (g_jit) snx_jit_remove_image(g_jit, base);
    pm_lock(&g_jit_synthetic_lock);
    for (i = 0; i < SNX_JIT_IMAGES; ++i)
        if (g_jit_synthetic[i].text_size && g_jit_synthetic[i].base == base) g_jit_synthetic[i].text_size = 0;
    pm_unlock(&g_jit_synthetic_lock);
}

/* a mapping, permission or write-back change under guest memory (mem.c): the slots the range's variants filled are
   stale, the JIT's code is stale, and a block the game mapped stops being code. The call is made with no memory lock
   held */
void jit_code_invalidate(u64 addr, u64 size) {
    MemBlock block;
    u64 change_lo = addr, change_hi = size > ~(u64)0 - addr ? ~(u64)0 : addr + size;
    u64 lo = change_lo < g_code_lo ? g_code_lo : change_lo;
    u64 hi = change_hi > g_code_hi ? g_code_hi : change_hi;
    u64 at;
    size_t i;
    if (!g_jit_images) return;
    if (lo & 3) lo = (lo + 3) & ~(u64)3;
    for (at = lo; at < hi; at += 4)
        __atomic_store_n(&g_disp[(at - g_code_lo) >> 2], NULL, __ATOMIC_RELEASE);
    if (g_jit) snx_jit_invalidate(g_jit, addr, size);
    pm_lock(&g_jit_synthetic_lock);
    for (i = 0; i < SNX_JIT_IMAGES; ++i) {
        SnxCodeImage* image = &g_jit_synthetic[i];
        if (!image->text_size || image->base + image->text_size <= change_lo || change_hi <= image->base) continue;
        if (code_at(image->base, &block)) continue;
        if (g_jit) snx_jit_remove_image(g_jit, image->base); /* before the memory can be reused or unmapped */
        image->text_size = 0;
    }
    pm_unlock(&g_jit_synthetic_lock);
    hook_restore(addr, size); /* the invalidation emptied the hooks' slots in the range too */
}

/* The AOT functions in the order of the lowest guest address each was compiled from (SnxModule.fn_lo), and the
   longest one's span: jit_code_changed's search, made at its first call, which most games never make. Under
   g_fn_lock */
static u32* g_fn_order;
static u64 g_fn_span;
static PMutex g_fn_lock = PMUTEX_INIT;
static int fn_lo_cmp(const void* a, const void* b) {
    u64 x = g_mod->fn_lo[*(const u32*)a], y = g_mod->fn_lo[*(const u32*)b];
    return x < y ? -1 : x > y;
}
static int fn_order(void) {
    u32 i, n = g_mod->nfns;
    if (g_fn_order) return 0;
    if (!n || !(g_fn_order = (u32*)malloc((size_t)n * sizeof *g_fn_order))) return -1;
    for (i = 0; i < n; i++) {
        g_fn_order[i] = i;
        if (g_mod->fn_len[i] > g_fn_span) g_fn_span = g_mod->fn_len[i];
    }
    qsort(g_fn_order, n, sizeof *g_fn_order, fn_lo_cmp);
    return 0;
}
/* AOT function i goes stale. Its dispatch slots and its entries the JIT calls it through (SnxModule.entry_fn, the
   module's own table, which it filled at load) go first, so a thread that reads its byte set finds no code for the
   entry it was called at, and the lookup compiles the code there now. An inlined copy reads the same byte */
static void fn_stale(const SnxModule* mod, u32 i) {
    GuestFn f = mod->fn[i], *entry_fn = (GuestFn*)(uintptr_t)mod->entry_fn;
    u64 lo = mod->fn_lo[i], hi = lo + mod->fn_len[i], pc;
    size_t a = 0, b = mod->nentries, m;
    for (pc = lo; pc < hi; pc += 4)
        if (pc >= g_code_lo && pc < g_code_hi) {
            GuestFn expected = f;
            __atomic_compare_exchange_n(&g_disp[(pc - g_code_lo) >> 2], &expected, NULL, 0, __ATOMIC_RELEASE,
                                        __ATOMIC_RELAXED);
        }
    while (a < b) {
        m = a + (b - a) / 2;
        if (mod->entry_addr[m] < lo)
            a = m + 1;
        else
            b = m;
    }
    for (; a < mod->nentries && mod->entry_addr[a] < hi; a++)
        if (__atomic_load_n(&entry_fn[a], __ATOMIC_RELAXED) == f) __atomic_store_n(&entry_fn[a], NULL, __ATOMIC_RELEASE);
    __atomic_store_n(&mod->stale[i], 1, __ATOMIC_RELEASE);
}

/* The NROs' own AOT modules, by their place in g_nros (nro_slots.h): each loaded when its NRO is first mapped at its
   address, and kept for its next mapping, as a thread may still be in its code when the NRO is unmapped (the JIT
   frees none either). mapped: its entries are in the dispatch table. refused: its file is no module of that NRO, said
   once. Under g_fn_lock */
typedef const SnxModule* (*NroModuleFn)(void);
typedef struct {
    void* library;
    NroModuleFn get;
    const SnxModule* module;
    const SnxNroImports* imports; /* NULL for a module that imports nothing */
    int mapped, refused;
} NroModule;
static NroModule* g_nro_modules;

/* An import of an NRO's module that has no function of the game's to call: through the dispatch table, where a hook,
   the JIT's code or RtAPI.miss answers for the address */
static void nro_import_dynamic(CPU* c, u64 pc) { snx_lookup(c, pc)(c, pc); }
/* The NRO module's imports (its snx_imports, nro_slots.h) each bound to the function that answers its address: what
   its dispatch slot holds, when that is the game module's entry there, or the function the runtime gave the slot in
   its place, a hook's or NVN's, which wins over the game's own. One given such a function later is bound to it then
   (jit_slot_taken). A slot that holds anything else can change again without a word, and keeps the call going through
   the table: code the JIT made, a profile pack's variant, or nothing (a function gone stale, or never compiled).
   Said: how many are direct, and how many of those the runtime's own. Under g_fn_lock */
static const SnxNroImports* nro_imports_bind(void* library, u64 base) {
    typedef const SnxNroImports* (*ImportsFn)(void);
    const SnxNroImports* im;
    ImportsFn get;
    void* sym = plat_dl_sym(library, "snx_imports");
    u32 i, direct = 0, own = 0;
    if (!sym) return NULL;
    memcpy(&get, &sym, sizeof get);
    im = get();
    if (!im || im->version != SNX_NRO_IMPORTS_VERSION) {
        WARN("jit: the NRO at %llx has an AOT module whose imports are of version %u, where this runtime reads %u: its "
             "calls of the game's functions go through the dispatch table\n",
             (unsigned long long)base, im ? (unsigned)im->version : 0u, (unsigned)SNX_NRO_IMPORTS_VERSION);
        return NULL;
    }
    for (i = 0; i < im->count; i++) {
        u64 a = im->address[i];
        GuestFn f = nro_import_dynamic, entry = NULL, held = NULL;
        size_t lo = 0, hi = g_mod->nentries;
        while (lo < hi) {
            size_t middle = lo + (hi - lo) / 2;
            if (g_mod->entry_addr[middle] < a)
                lo = middle + 1;
            else
                hi = middle;
        }
        if (lo < g_mod->nentries && g_mod->entry_addr[lo] == a)
            entry = __atomic_load_n(&g_mod->entry_fn[lo], __ATOMIC_ACQUIRE);
        if (a >= g_code_lo && a < g_code_hi && !(a & 3))
            held = __atomic_load_n(&g_disp[(a - g_code_lo) >> 2], __ATOMIC_ACQUIRE);
        if (held && held == entry) {
            f = held;
            direct++;
        } else if (held && (held == hook_function(a) || nvn_loader_handler(held))) {
            f = held;
            direct++;
            own++;
        }
        __atomic_store_n((GuestFn*)&im->function[i], f, __ATOMIC_RELEASE);
    }
    LOG("jit: the NRO at %llx calls %u of the %u functions of the game its module imports directly, %u of them the "
        "runtime's own in the game's place (hooks, NVN)\n",
        (unsigned long long)base, (unsigned)direct, (unsigned)im->count, (unsigned)own);
    return im;
}

/* the module of the NRO at place, loaded, or NULL: no file (said nothing, the JIT's line follows), or refused */
static const SnxModule* nro_module_load(long place, u64 base, u64 size, const u8* id, int* refused) {
    static const char digits[] = "0123456789abcdef";
    NroModule* n = &g_nro_modules[place];
    const SnxModule* m;
    const SnxModInfo* info;
    char path[1400], name[65];
    void* sym;
    size_t i;
    if (n->refused) return NULL;
    for (i = 0; i < 32; i++) {
        name[2 * i] = digits[g_nros->slots[place].hash[i] >> 4];
        name[2 * i + 1] = digits[g_nros->slots[place].hash[i] & 15];
    }
    name[64] = 0;
#if defined(_WIN32) /* a backslash, as the system's loader asks of a path */
    snprintf(path, sizeof path, "%s%s" SNX_NROS_FOLDER "\\%s.dll", g_exedir, g_exename, name);
#else
    snprintf(path, sizeof path, "%s%s" SNX_NROS_FOLDER "/%s.so", g_exedir, g_exename, name);
#endif
    if (!n->library) {
        if (!snx_is_file(path)) return NULL;
        n->library = plat_dl_open(path);
        if (!n->library) {
            WARN("jit: the NRO at %llx has an AOT module that could not be loaded (%s): the JIT runs its code\n",
                 (unsigned long long)base, plat_dl_error());
            LOG("jit: that module is %s\n", path);
            *refused = n->refused = 1;
            return NULL;
        }
        sym = plat_dl_sym(n->library, "snx_module");
        if (sym) memcpy(&n->get, &sym, sizeof n->get); /* ISO C has no conversion from an object pointer */
    }
    /* each mapping starts from the module as it was made: its entries filled again, none of its functions stale */
    m = n->get ? n->get() : NULL;
    info = m && m->abi == SNX_ABI && m->nmods == 1 ? &m->mods[0] : NULL;
    if (!info || info->base != base || info->size != size || memcmp(info->build_id, id, 32) || m->code_lo < base ||
        m->code_hi > base + size) {
        if (!m || m->abi != SNX_ABI)
            WARN("jit: the NRO at %llx has an AOT module of ABI %u, where this runtime runs %u: the JIT runs its code "
                 "until the game is recompiled\n",
                 (unsigned long long)base, m ? (unsigned)m->abi : 0u, (unsigned)SNX_ABI);
        else if (!info || info->base != base || info->size != size || m->code_lo < base || m->code_hi > base + size)
            WARN("jit: the NRO at %llx (%llu bytes) has an AOT module made for %llx (%llu bytes, %u modules): the JIT "
                 "runs its code until the game is recompiled\n",
                 (unsigned long long)base, (unsigned long long)size, info ? (unsigned long long)info->base : 0ull,
                 info ? (unsigned long long)info->size : 0ull, (unsigned)m->nmods);
        else
            WARN("jit: the NRO at %llx has an AOT module made from another NRO (module ID %02x%02x%02x%02x..., where "
                 "the mapped one's is %02x%02x%02x%02x...): the JIT runs its code until the game is recompiled\n",
                 (unsigned long long)base, info->build_id[0], info->build_id[1], info->build_id[2], info->build_id[3],
                 id[0], id[1], id[2], id[3]);
        LOG("jit: that module is %s\n", path);
        plat_dl_close(n->library);
        n->library = NULL;
        n->get = NULL;
        n->module = NULL;
        n->imports = NULL;
        *refused = n->refused = 1;
        return NULL;
    }
    if (m->nfns) memset(m->stale, 0, m->nfns);
    n->imports = nro_imports_bind(n->library, base);
    n->module = m;
    return m;
}

int jit_nro_added(const u8* hash, u64 base, u64 size, const u8* id, u32* entries) {
    long place = snx_nro_slot(g_nros, hash);
    const SnxModule* m;
    int refused = 0;
    u32 i, n = 0;
    *entries = 0;
    if (!NRO_MODULES || place < 0 || g_nros->slots[place].base != base || g_nros->slots[place].size != size) return 1;
    if (!g_disp || base < g_code_lo || size > g_code_hi - base) return 1;
    pm_lock(&g_fn_lock);
    if (!g_nro_modules) g_nro_modules = (NroModule*)calloc(g_nros->count, sizeof *g_nro_modules);
    m = g_nro_modules ? nro_module_load(place, base, size, id, &refused) : NULL;
    if (m) {
        /* a slot something holds already (a hook, code the JIT made while the NRO had no module) keeps it */
        for (i = 0; i < m->nentries; i++) {
            u64 a = m->entry_addr[i];
            GuestFn expected = NULL;
            if (a < base || a >= base + size || (a & 3) || !m->entry_fn[i]) continue;
            n += (u32)__atomic_compare_exchange_n(&g_disp[(a - g_code_lo) >> 2], &expected, m->entry_fn[i], 0,
                                                  __ATOMIC_RELEASE, __ATOMIC_RELAXED);
        }
        g_nro_modules[place].mapped = 1;
        *entries = n;
    }
    pm_unlock(&g_fn_lock);
    return m ? 0 : refused ? -1 : 1;
}

void jit_slot_taken(u64 addr, GuestFn fn) {
    u32 k;
    if (!g_nros) return;
    /* after the slot's store: a bind that read the slot before it is under this lock, and is made again here */
    pm_lock(&g_fn_lock);
    for (k = 0; g_nro_modules && k < g_nros->count; k++) {
        const SnxNroImports* im = g_nro_modules[k].imports;
        size_t lo = 0, hi = im ? im->count : 0;
        while (lo < hi) {
            size_t middle = lo + (hi - lo) / 2;
            if (im->address[middle] < addr)
                lo = middle + 1;
            else
                hi = middle;
        }
        if (im && lo < im->count && im->address[lo] == addr)
            __atomic_store_n((GuestFn*)&im->function[lo], fn, __ATOMIC_RELEASE);
    }
    pm_unlock(&g_fn_lock);
}

void jit_nro_removed(u64 base) {
    u32 k, i;
    pm_lock(&g_fn_lock);
    for (k = 0; g_nro_modules && k < g_nros->count; k++) {
        const SnxModule* m = g_nro_modules[k].module;
        if (!g_nro_modules[k].mapped || g_nros->slots[k].base != base) continue;
        for (i = 0; i < m->nentries; i++) {
            u64 a = m->entry_addr[i];
            GuestFn expected = m->entry_fn[i];
            if (a < g_code_lo || a >= g_code_hi || (a & 3) || !expected) continue;
            __atomic_compare_exchange_n(&g_disp[(a - g_code_lo) >> 2], &expected, NULL, 0, __ATOMIC_RELEASE,
                                        __ATOMIC_RELAXED);
        }
        g_nro_modules[k].mapped = 0;
    }
    pm_unlock(&g_fn_lock);
}
void jit_code_changed(u64 addr, u64 size) {
    u64 end = addr + size, from;
    size_t a = 0, b, m;
    u32 k;
    pm_lock(&g_fn_lock);
    if (g_mod->nfns && !fn_order()) {
        from = addr > g_fn_span ? addr - g_fn_span : 0;
        b = g_mod->nfns;
        while (a < b) {
            m = a + (b - a) / 2;
            if (g_mod->fn_lo[g_fn_order[m]] < from)
                a = m + 1;
            else
                b = m;
        }
        for (; a < g_mod->nfns && g_mod->fn_lo[g_fn_order[a]] < end; a++) {
            u32 i = g_fn_order[a];
            if (g_mod->fn_lo[i] + g_mod->fn_len[i] <= addr || __atomic_load_n(&g_mod->stale[i], __ATOMIC_RELAXED))
                continue;
            fn_stale(g_mod, i);
            LOG_LIMIT(64, "jit: the game changed its code at %llx, compiled into its function from %llx: compiled again\n",
                      (unsigned long long)addr, (unsigned long long)g_mod->fn_lo[i]);
        }
    }
    /* the same for a mapped NRO's own module: its functions one by one, as few games change an NRO's code */
    for (k = 0; g_nro_modules && k < g_nros->count; k++) {
        const SnxModule* m = g_nro_modules[k].module;
        u32 i;
        if (!g_nro_modules[k].mapped || g_nros->slots[k].base >= end ||
            g_nros->slots[k].base + g_nros->slots[k].size <= addr)
            continue;
        for (i = 0; i < m->nfns; i++) {
            if (m->fn_lo[i] >= end || m->fn_lo[i] + m->fn_len[i] <= addr ||
                __atomic_load_n(&m->stale[i], __ATOMIC_RELAXED))
                continue;
            fn_stale(m, i);
            LOG_LIMIT(64,
                      "jit: the game changed its code at %llx, compiled into its function from %llx: compiled again\n",
                      (unsigned long long)addr, (unsigned long long)m->fn_lo[i]);
        }
    }
    pm_unlock(&g_fn_lock);
    jit_code_invalidate(addr, size); /* the slots in the line, and the JIT's code there */
}

void jit_report(void) {
    unsigned long images = g_mod ? g_mod->nmods : 0;
    size_t i;
    for (i = 0; i < SNX_JIT_IMAGES; ++i)
        if (g_jit_synthetic[i].text_size) ++images;
    LOG("jit: %u AOT variants, %zu leaders, %lu hits, %lu misses, %lu images\n",
        g_variants ? g_variants->count : 0, g_variant_leader_count, __atomic_load_n(&g_jit_hits, __ATOMIC_RELAXED),
        __atomic_load_n(&g_jit_misses, __ATOMIC_RELAXED), images);
    if (g_jit) {
        SnxJitStats stats;
        snx_jit_stats(g_jit, &stats);
        LOG("jit: %lu batches compiled %lu pcs (%lu failed, %lu still queued, %lu batches dropped by memory changes), "
            "%lu functions, %lu instructions, %lu KiB of code, %.2f s compiling, %lu units loaded from %s\n",
            stats.batches, stats.compiled, stats.failures, stats.queued, stats.dropped, stats.functions,
            stats.instructions, (unsigned long)(stats.bytes >> 10), stats.seconds, stats.cached, g_jit_cache);
    } else {
        LOG("jit: no JIT: %s\n", g_jit_error[0] ? g_jit_error : "not started");
    }
}
