/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "3ds/runtime/internal.h"
#include "common/utilities/endian.h"
#include "3ds/gpu/pica.h"
#include "3ds/services/dsp.h"
#include "3ds/services/ptm.h"
#include "3ds/services/file.h"
#include "3ds/services/uds.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "common/utilities/common.h"
static bool map(SnxCtrProcess* p, uint32_t va, uint32_t size, unsigned permissions) {
    if (!size) return true;
    if ((uint64_t)p->physical_next + size > p->physical_end ||
        snx_ctr_map(p->space, va, p->physical_next, size, permissions) != SNX_CTR_MEM_OK)
        return false;
    memset(p->physical_owner + (p->physical_next - CTR_PHYS_BASE) / 4096, CTR_STATIC, size / 4096);
    p->physical_next += size;
    return true;
}
/* The ExHeader's address-mapping capabilities, as Azahar's Process::ParseKernelCaps and
   KernelSystem::HandleSpecialMapping (core/hle/kernel/{process,memory}.cpp, GPL-2.0-or-later, Copyright 2014-2025
   Citra Emulator Project / Azahar Emulator Project, azahar-emu/azahar a4b335e) read and apply them: a range is two
   descriptors (type 0x7f, start page, read-only bit 20, then the end page), a page is one (type 0xffe). Mappings
   resolve against the special areas in that order, VRAM before IO (their virtual ranges overlap). MMIO areas are
   not mapped, as Azahar leaves them, so an access to one faults instead of reading invented values. */
static bool special_mappings(SnxCtrProcess* p, const uint32_t* caps) {
    static const struct {
        uint32_t va, pa, size;
    } areas[] = {
        {0x1f000000u, 0x18000000u, 0x00600000u},            /* VRAM */
        {0x1ec00000u, 0x10100000u, 0x00400000u},            /* IO: not mapped */
        {0x1ff00000u, 0x1ff00000u, 0x00080000u},            /* DSP RAM */
        {0x1e800000u, 0x1f000000u, 0x00400000u - 0x20000u}, /* New 3DS extra RAM */
    };
    for (unsigned i = 0; i < 28; ++i) {
        uint32_t d = caps[i], type = d >> 20, va, size, permissions;
        if ((type & 0xffe) == 0xff8) {
            if (i + 1 >= 28 || ((caps[i + 1] >> 20) & 0xffe) != 0xff8) continue; /* incomplete: ignored */
            uint32_t end = caps[++i] << 12;
            va = d << 12;
            size = va < end ? end - va : 0;
            permissions = (d & (1u << 20)) ? SNX_CTR_READ : SNX_CTR_READ | SNX_CTR_WRITE;
        } else if ((type & 0xfff) == 0xffe) {
            va = d << 12;
            size = 4096;
            permissions = SNX_CTR_READ | SNX_CTR_WRITE;
        } else
            continue;
        if (!size) continue;
        unsigned area = 0;
        while (area < 4 &&
               !(va >= areas[area].va && (uint64_t)va + size <= (uint64_t)areas[area].va + areas[area].size))
            ++area;
        if (area == 4 || area == 1) continue; /* unhandled or MMIO */
        uint32_t pa = areas[area].pa + (va - areas[area].va);
        /* an area this board lacks (extra RAM on an Old 3DS) has no physical region: left unmapped */
        if (!snx_ctr_physical_region(snx_ctr_memory_profile(p->memory), pa, size)) continue;
        if (snx_ctr_map(p->space, va, pa, size, permissions) != SNX_CTR_MEM_OK) return false;
    }
    return true;
}
/* The host's local wall-clock time in milliseconds since 1900, the shared page's date_time: the console keeps local
   time, and Azahar gives it the host's (Handler::GetSystemTimeSince1900, daylight saving included). The console
   allows no time before 2000, so an earlier host clock reads as 2000-01-01 as Azahar has it. */
static bool local_1900_ms(uint64_t* out) {
    struct timespec now;
    struct tm local;
    if (
#if defined(__ANDROID__) && __ANDROID_API__ < 29
        clock_gettime(CLOCK_REALTIME, &now)
#else
        !timespec_get(&now, TIME_UTC)
#endif
    )
        return false;
    time_t seconds = now.tv_sec;
#ifdef _WIN32
    if (localtime_s(&local, &seconds)) return false;
#else
    if (!localtime_r(&seconds, &local)) return false;
#endif
    /* days from 1900-01-01 to the local date (the proleptic Gregorian count of H. Hinnant's days_from_civil) */
    int64_t year = (int64_t)local.tm_year + 1900 - (local.tm_mon < 2);
    int64_t era = (year >= 0 ? year : year - 399) / 400, of_era = year - era * 400;
    int64_t month = local.tm_mon < 2 ? local.tm_mon + 10 : local.tm_mon - 2; /* from March */
    int64_t of_year = (153 * month + 2) / 5 + local.tm_mday - 1;
    int64_t days = era * 146097 + of_era * 365 + of_era / 4 - of_era / 100 + of_year - 719468 + 25567;
    int64_t ms = ((days * 24 + local.tm_hour) * 60 + local.tm_min) * 60 + local.tm_sec;
    ms = ms * 1000 + now.tv_nsec / 1000000;
    *out = ms > INT64_C(3155673600000) ? (uint64_t)ms : UINT64_C(3155673600000);
    return true;
}
void snx_ctr_shared_page_time(SnxCtrProcess* p) {
    /* Azahar's UpdateTimeCallback: the entry the counter's parity does not select gets the time, the tick it was
       taken at and the tick rate; then the counter moves on, so readers (index counter & 1) switch to it whole */
    uint64_t rtc, ticks = (uint64_t)((snx_seconds() - p->clock_origin) * 268111856.0), values[4];
    p->time_update_ns = snx_ctr_now_ns() + UINT64_C(3600000000000);
    unsigned char counter[4];
    if (!local_1900_ms(&rtc) || snx_ctr_phys_read(p->memory, 0x1ff81000, counter, 4) != SNX_CTR_MEM_OK) return;
    uint32_t count = snx_ld_le32(counter);
    values[0] = rtc;
    values[1] = ticks;
    values[2] = UINT64_C(268111856);
    values[3] = 0;
    unsigned char entry[32];
    for (unsigned v = 0; v < 4; ++v) snx_st_le64(entry + v * 8, values[v]);
    if (snx_ctr_phys_write(p->memory, 0x1ff81020 + (count % 2 ? 0 : 0x20), entry, sizeof entry) != SNX_CTR_MEM_OK)
        return;
    ++count;
    snx_st_le32(counter, count);
    (void)snx_ctr_phys_write(p->memory, 0x1ff81000, counter, 4);
}
void snx_ctr_process_free(SnxCtrProcess* p) {
    if (!p) return;
    snx_ctr_uds_close(p);
    if (p->gsp.pica) snx_ctr_pica_free(p->gsp.pica);
    free(p->gsp.pica);
    snx_ctr_dsp_close(p);
    /* files still open when the process ends close their host files and release their nodes before the tree goes */
    for (uint32_t i = 1; i < CTR_OBJECTS; ++i)
        if (p->objects[i].type == CTR_OBJ_FILE && !p->objects[i].data.file.closed) snx_ctr_file_release(p, i);
    for (unsigned i = 0; i < CTR_SELF_ARCHIVES; ++i) /* and so do the ExtSaveData archives still open */
        if (p->self_archives[i].root) fs_tree_release(p->self_archives[i].root);
    snx_ctr_romfs_free(p->romfs);
    if (p->tree) {
        fs_tree_destroy(p->tree);
        pm_destroy(&p->tree->lock);
        free(p->tree);
    }
    snx_ctr_space_free(p->space);
    snx_ctr_memory_free(p->memory);
    free(p->aot_stale);
    free(p);
}
SnxCtrProcess* snx_ctr_process_new(const SnxCtrImage* image, SnxCtrModel model) {
    const SnxCtrProfile* profile = snx_ctr_profile(model);
    if (!image || !image->code || !profile || !image->stack_size || image->stack_size > 0x800000) return NULL;
    uint32_t app_limit = model == SNX_CTR_OLD ? 0x4000000 : 0x7c00000;
    SnxCtrProcess* p = calloc(1, sizeof *p + app_limit / 4096);
    if (!p) return NULL;
    p->app_limit = app_limit;
    memcpy(p->segment, image->segment, sizeof p->segment);
    p->bss_size = image->bss_size;
    p->title_id = image->title_id;
    memcpy(p->name, image->name, sizeof p->name);
    p->gsp.rights_thread = UINT32_MAX; /* no GPU right holder until an AcquireRight */
    p->fs_priority = UINT32_MAX;       /* fs:USER's, until a SetPriority (Azahar's starts at -1) */
    p->physical_next = CTR_PHYS_BASE;
    p->physical_end = p->physical_next + p->app_limit;
    p->objects[1] = (CtrObject){.type = CTR_OBJ_PROCESS, .refs = 1};
    p->objects[2] = (CtrObject){.type = CTR_OBJ_THREAD, .refs = 1};
    p->current_thread = 2;
    /* the ExHeader's ARM11 local capabilities give the main thread's priority (Azahar Process::Run) */
    p->objects[2].data.thread.priority = image->main_priority;
    p->memory = snx_ctr_memory_new(model);
    if (!p->memory || !(p->space = snx_ctr_space_new(p->memory))) goto fail;
    size_t offset = 0;
    for (unsigned i = 0; i < 3; ++i) {
        const SnxCtrSegment* s = &image->segment[i];
        uint64_t bytes = (uint64_t)s->pages * 4096;
        uint64_t size = bytes + (i == 2 ? ((uint64_t)image->bss_size + 4095) & ~4095ull : 0);
        if (size > UINT32_MAX || bytes > image->code_size - offset ||
            !map(p, s->address, (uint32_t)size, SNX_CTR_READ | SNX_CTR_WRITE))
            goto fail;
        if (bytes && snx_ctr_write(p->space, s->address, image->code + offset, (size_t)bytes) != SNX_CTR_MEM_OK)
            goto fail;
        unsigned permissions = i == 0   ? SNX_CTR_READ | SNX_CTR_EXEC
                               : i == 1 ? SNX_CTR_READ
                                        : SNX_CTR_READ | SNX_CTR_WRITE;
        if (size && snx_ctr_protect(p->space, s->address, (uint32_t)size, permissions) != SNX_CTR_MEM_OK) goto fail;
        offset += (size_t)bytes;
    }
    uint32_t stack = (image->stack_size + 4095) & ~4095u;
    p->stack_size = stack;
    /* the main thread's TLS page is the BASE region's, outside the application's pool, as every TLS page is
       (kernel/thread.c) */
    uint32_t tls_page = 0;
    if (offset != image->code_size || !map(p, SNX_CTR_STACK_TOP - stack, stack, SNX_CTR_READ | SNX_CTR_WRITE) ||
        !(tls_page = snx_ctr_shared_pages(p, 4096)) ||
        snx_ctr_map(p->space, SNX_CTR_INITIAL_TLS, tls_page, 4096, SNX_CTR_READ | SNX_CTR_WRITE) != SNX_CTR_MEM_OK ||
        !snx_ctr_cpu_init(&p->cpu, p->space, image->segment[0].address, SNX_CTR_STACK_TOP, false))
        goto fail;
    p->cpu.state.cpu.tpidruro = SNX_CTR_INITIAL_TLS;
    /* default NaN, flush-to-zero, round toward zero and the inexact enable, as Azahar's SetupMainThread starts the main
       thread (kernel/thread.cpp); created threads start without the inexact bit (snx_ctr_thread_create) */
    p->cpu.state.cpu.fpscr = 0x03c00010u;
    p->objects[2].data.thread.context = p->cpu.state.cpu;
    /* Configuration-page layout and kernel personality facts from Citra/Azahar
       config_mem.{h,cpp}, memory.cpp (GPL-2.0-or-later, revision in docs/3DS.md).
       No firmware is loaded. User mappings are read-only. */
    /* Board AXI WRAM, not application FCRAM: do not charge this kernel page
       to the application's heap quota or alias it to application allocations. */
    if (snx_ctr_map(p->space, CTR_CONFIG, CTR_CONFIG, 4096, SNX_CTR_READ | SNX_CTR_WRITE) != SNX_CTR_MEM_OK) goto fail;
    unsigned char config[4096] = {0};
    config[2] = config[0x62] = 0x3a;
    config[3] = config[0x63] = 2;
    config[0x14] = 1;
    config[0x16] = 1;
    config[0x10] = config[0x64] = 2;
    config[0x18] = config[0x68] = 0x50;
    config[0x19] = config[0x69] = 0xf4;
    uint32_t sizes[] = {p->app_limit, model == SNX_CTR_OLD ? 0x2c00000u : 0x6400000u,
                        model == SNX_CTR_OLD ? 0x1400000u : 0x2000000u};
    config[0x30] = model == SNX_CTR_OLD ? 0 : 6;
    for (unsigned i = 0; i < 3; ++i)
        for (unsigned j = 0; j < 4; ++j) config[0x40 + i * 4 + j] = (unsigned char)(sizes[i] >> (j * 8));
    if (snx_ctr_write_ram(p->space, CTR_CONFIG, config, sizeof config) != SNX_CTR_MEM_OK ||
        snx_ctr_protect(p->space, CTR_CONFIG, 4096, SNX_CTR_READ) != SNX_CTR_MEM_OK)
        goto fail;
    /* The shared page (Azahar kernel/shared_page.{h,cpp}, GPL-2.0-or-later): a retail console (running_hw 1),
       unknown_value 1 as Azahar sets it, a full battery on its charger, Wi-Fi at its best link with Internet, the 3D
       slider down. The time entry is written now and then every hour; clients interpolate between with SVC 0x28.
       Host-paced monotonic ticks, not an instruction/cycle-accuracy claim. */
    uint64_t rtc;
    if (!local_1900_ms(&rtc)) goto fail;
    p->clock_origin = snx_seconds();
    unsigned char shared[4096] = {0};
    shared[0x04] = 1;    /* running_hw: product */
    shared[0x66] = 3;    /* wifi_link_level: best */
    shared[0x67] = 2;    /* wifi_state: Internet */
    shared[0x85] = 0x17; /* battery_state: adapter connected, charging, completely full (5) */
    shared[0x86] = 1;    /* unknown_value */
    if (snx_ctr_map(p->space, 0x1ff81000, 0x1ff81000, 4096, 3) != SNX_CTR_MEM_OK ||
        snx_ctr_write_ram(p->space, 0x1ff81000, shared, sizeof shared) != SNX_CTR_MEM_OK ||
        snx_ctr_protect(p->space, 0x1ff81000, 4096, SNX_CTR_READ) != SNX_CTR_MEM_OK)
        goto fail;
    snx_ctr_shared_page_time(p);
    if (!special_mappings(p, image->kernel_caps)) goto fail;
    return p;
fail:
    snx_ctr_process_free(p);
    return NULL;
}
/* One NX-style shared tree over the extracted title directory: romfs and
   exefs are shared read-only and always open; sdmc is the read-write area,
   created on demand. The process owns the tree. */
static bool mount(SnxCtrProcess* p, const char* directory, const SnxCtrSource* game);
bool snx_ctr_process_mount_fs(SnxCtrProcess* p, const char* directory) { return mount(p, directory, NULL); }
/* Without an extraction: the tree is the title's state folder (sdmc/ and what the driver keeps there), and the RomFS
   is the game's level 3 read where it lies */
bool snx_ctr_process_mount_source(SnxCtrProcess* p, const char* directory, const SnxCtrSource* game) {
    return game && mount(p, directory, game);
}
/* The SMDH's short title, for the window: one of the 16 languages' 0x200-byte entries (short title, 0x40 UTF-16LE
   units, then the long one and the publisher), English (1) first, then the others in order, as UTF-8 */
static void smdh_title(const unsigned char* smdh, char out[192]) {
    out[0] = 0;
    for (unsigned order = 0; order < 16 && !out[0]; ++order) {
        const unsigned char* s = smdh + 8 + 0x200 * (order == 0 ? 1 : order == 1 ? 0 : order);
        size_t n = 0;
        for (unsigned i = 0; i < 0x40; ++i) {
            uint32_t cp = (uint32_t)s[i * 2] | (uint32_t)s[i * 2 + 1] << 8;
            if (!cp) break;
            if (cp >= 0xd800 && cp < 0xdc00 && i + 1 < 0x40) { /* a surrogate pair */
                uint32_t low = (uint32_t)s[i * 2 + 2] | (uint32_t)s[i * 2 + 3] << 8;
                if (low >= 0xdc00 && low < 0xe000) {
                    cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
                    ++i;
                }
            }
            if (cp == '\n') cp = ' '; /* a title broken over two lines is one in a window's */
            unsigned char u[4];
            unsigned len = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
            if (len == 1)
                u[0] = (unsigned char)cp;
            else {
                for (unsigned k = len - 1; k > 0; --k, cp >>= 6) u[k] = (unsigned char)(0x80 | (cp & 0x3f));
                u[0] = (unsigned char)((len == 2 ? 0xc0 : len == 3 ? 0xe0 : 0xf0) | cp);
            }
            if (n + len >= 192) break;
            memcpy(out + n, u, len);
            n += len;
        }
        out[n] = 0;
    }
}
static bool mount(SnxCtrProcess* p, const char* directory, const SnxCtrSource* game) {
    if (!p || !directory || !*directory || p->tree) return false;
    FsTree* tree = calloc(1, sizeof *tree);
    if (!tree) return false;
    if (fs_tree_init(tree, directory) != FS_TREE_OK) {
        pm_destroy(&tree->lock);
        free(tree);
        return false;
    }
    p->tree = tree;
    pm_lock(&tree->lock);
    FsShare share = {0};
    bool ok = true;
    for (unsigned i = 0; i < 2 && ok && !game; ++i) {
        FsNode* node = NULL;
        if (!fs_tree_lookup(tree, NULL, i ? "exefs" : "romfs", &node)) ok = !fs_tree_share(tree, node, &share);
    }
    pm_unlock(&tree->lock);
    if (game) {
        uint64_t size;
        const unsigned char* image = snx_ctr_source_romfs(game, &size);
        /* a CIA without a RomFS reads as an empty one, as its extraction's empty romfs/ folder does */
        ok = ok && snx_ctr_romfs_open_image(image, size, snx_ctr_source_mtime(game), &p->romfs);
        /* the ExeFS files SelfNCCH serves (services/fs.c), in the tree as an extraction's exefs/ has them: copies of
           the CIA's sections, read-only and never on the host */
        static const char* const sections[] = {"icon", "logo", "banner"};
        pm_lock(&tree->lock);
        for (unsigned i = 0; i < 3 && ok; ++i) {
            const unsigned char* data = snx_ctr_source_exefs(game, sections[i], &size);
            char path[16];
            snprintf(path, sizeof path, "exefs/%s", sections[i]);
            if (data) ok = !fs_tree_add_virtual(tree, path, data, size, NULL);
        }
        pm_unlock(&tree->lock);
    } else if (!ok || !snx_ctr_romfs_open(tree, &p->romfs))
        ok = false;
    FsNode* sd = NULL;
    if (ok) {
        pm_lock(&tree->lock);
        FsTreeResult rc = fs_tree_lookup(tree, NULL, "sdmc", &sd);
        if (rc == FS_TREE_NOTFOUND) rc = fs_tree_create(tree, NULL, "sdmc", 1, 0, &sd);
        pm_unlock(&tree->lock);
        ok = !rc && sd;
    }
    if (!ok) { /* leave no partial mount behind; the process itself stays the caller's */
        snx_ctr_romfs_free(p->romfs);
        p->romfs = NULL;
        fs_tree_destroy(p->tree);
        pm_destroy(&p->tree->lock);
        free(p->tree);
        p->tree = NULL;
        p->sd = NULL;
        return false;
    }
    p->sd = sd;
    /* the regions the title's SMDH (ExeFS icon) says it runs in, which pick the console's (services/cfg.c) */
    unsigned char smdh[0x2020];
    uint64_t smdh_size = 0;
    const unsigned char* icon = game ? snx_ctr_source_exefs(game, "icon", &smdh_size) : NULL;
    if (!game) {
        char* path = snx_path_join(directory, "exefs/icon");
        FILE* f = path ? fopen(path, "rb") : NULL;
        if (f && fread(smdh, 1, sizeof smdh, f) == sizeof smdh) icon = smdh, smdh_size = sizeof smdh;
        if (f) fclose(f);
        free(path);
    }
    if (icon && smdh_size >= 0x201c && !memcmp(icon, "SMDH", 4)) smdh_title(icon, p->title);
    if (icon && smdh_size >= 0x201c && !memcmp(icon, "SMDH", 4))
        p->region_lockout = snx_ld_le32(icon + 0x2018);
    /* the save data a run formatted before */
    pm_lock(&tree->lock);
    if (fs_tree_lookup(tree, NULL, "savedata", &p->save) || !p->save->is_dir) p->save = NULL;
    pm_unlock(&tree->lock);
    snx_ctr_dsp_component_file(p);
    snx_ctr_ptm_system_data(p);
    return true;
}
/* HLE shared pages come from board FCRAM past the application pool, outside
   pool accounting: each call reserves the next page-aligned block. */
uint32_t snx_ctr_shared_pages(SnxCtrProcess* p, uint32_t size) {
    if (!p || !size || (size & 4095)) return 0;
    const SnxCtrProfile* profile = snx_ctr_memory_profile(p->memory);
    uint32_t limit = CTR_PHYS_BASE + profile->fcram_size;
    if (!p->physical_shared) p->physical_shared = p->physical_end;
    if ((uint64_t)p->physical_shared + size > limit) return 0;
    uint32_t at = p->physical_shared;
    p->physical_shared += size;
    return at;
}
SnxArmCPU* snx_ctr_process_cpu(SnxCtrProcess* p) { return p ? &p->cpu.state.cpu : NULL; }
SnxCtrAddressSpace* snx_ctr_process_space(SnxCtrProcess* p) { return p ? p->space : NULL; }
