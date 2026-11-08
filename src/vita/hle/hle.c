/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The imported functions by NID: each import of the image (vita/loader/image.h) is looked up in the modules' tables
   at its first call. A NID names one function wherever it is imported from, so the tables are by NID alone */
#include "vita/hle/hle.h"
#include "snx_settings.h"
#include <inttypes.h>
#include <string.h>
#include <stdio.h>

/* The two system variables that libc.suprx and the perf library import, which no loaded guest module exports:
   - SceLibcParam's __sce_libcparam (0xDF084DFA): a pointer to the SceLibcParam inside SceProcessParam. Vita3K's
     VAR_EXPORT in SceLibcParam returns `process_param->sce_libc_param.address()` (or 0 with no process param).
   - ScePerf's _pLibPerfCaptureFlagPtr (0x936A5F31): a pointer to a 4-byte capture flag. Vita3K's VAR_EXPORT in
     ScePerf allocates the flag (zeroed) and stores its address; profiling capture is never on, so the flag is 0. */
#define NID_SCE_LIBCPARAM     0xdf084dfau
#define NID_LIBPERF_FLAG_PTR  0x936a5f31u
#define NID_LIBC_TLS_GET_ADDR 0xbc529b7bu

uint32_t vita_arg(VitaProcess* p, VitaThread* t, unsigned n) {
    return n < 4 ? t->cpu.r[n] : vita_read32(p, t->cpu.r[13] + (n - 4) * 4);
}
static const VitaHle* find(uint32_t nid) {
    static const VitaHle* const tables[] = {vita_hle_kernel, vita_hle_io,    vita_hle_ctrl, vita_hle_display,
                                            vita_hle_audio,  vita_hle_misc, vita_hle_gxm, vita_hle_libc,
                                            vita_hle_fiber, vita_hle_ngs, vita_hle_mp4, vita_hle_videodec,
                                            vita_hle_audiodec, vita_hle_dialog, vita_hle_net,
                                            vita_hle_trophy};
    for (size_t i = 0; i < sizeof tables / sizeof *tables; ++i)
        for (const VitaHle* h = tables[i]; h->name; ++h)
            if (h->nid == nid) return h;
    return NULL;
}
static uint32_t vita_export_return(VitaProcess* p, VitaThread* t, void* context) {
    (void)p;
    (void)context;
    return t->cpu.r[0];
}
static const VitaExport* vita_export_find(const VitaImage* image, uint32_t library_nid, uint32_t nid) {
    for (uint32_t i = 0; i < image->export_count; ++i) {
        const VitaExport* e = &image->exports[i];
        if (!e->variable && e->library_nid == library_nid && e->nid == nid) return e;
    }
    return NULL;
}
/* The calls the trace (level 2) leaves out, which a run makes by the thousand and none of which says what the game is
   doing: the memory functions, the lightweight mutexes' locks, the thread and time queries. A call that waits is
   logged all the same */
static bool trace_quiet(const char* name) {
    static const char* const quiet[] = {"sceClibMemcmp", "sceClibMemcpy", "sceClibMemset", "sceClibMemmove", "memcmp",
                                        "memcpy", "memset", "memmove", "sceKernelLockLwMutex",
                                        "sceKernelUnlockLwMutex", "sceKernelUnlockLwMutex2", "sceKernelGetThreadId",
                                        "sceKernelGetProcessTimeWide", "sceKernelGetTLSAddr"};
    for (unsigned i = 0; i < sizeof quiet / sizeof *quiet; ++i)
        if (!strcmp(name, quiet[i])) return true;
    return false;
}
void vita_hle_call(VitaProcess* p, VitaThread* t, uint32_t index) {
    const VitaImport* import = &p->image.imports[index];
    ++p->hle_calls;
    uint32_t r[4] = {t->cpu.r[0], t->cpu.r[1], t->cpu.r[2], t->cpu.r[3]};
    /* SceLibc's resolver needs a registered module table. Handle the main image's TLS directly; the HLE libc copies
       its PT_TLS data per thread, while descriptors for other modules still use their guest export. */
    bool main_tls = import->nid == NID_LIBC_TLS_GET_ADDR && p->image.tls_memsz && vita_mapped(p, r[0], 8) &&
                    vita_read32(p, r[0]) == UINT32_MAX;
    if (!main_tls && !p->hle_export_known[index]) {
        p->hle_export[index] = vita_export_find(&p->image, import->library_nid, import->nid);
        p->hle_export_known[index] = 1;
    }
    const VitaExport* e = main_tls ? NULL : p->hle_export[index];
    if (e) {
        vita_hle_guest_call(p, t, e->address, r, 4, vita_export_return, NULL);
        if (g_settings.vita_import_trace > 1)
            printf("vita: \"%s\" %s NID 0x%08" PRIx32 " -> guest 0x%08" PRIx32 " (0x%" PRIx32 ", 0x%" PRIx32
                   ", 0x%" PRIx32 ", 0x%" PRIx32 ", lr 0x%08" PRIx32 ")\n", t->name, import->library, import->nid,
                   e->address, r[0], r[1], r[2], r[3], t->call.lr);
        return;
    }
    const VitaHle* h = p->hle[index];
    if (!h && !p->unknown_logged[index]) {
        h = find(import->nid);
        p->hle[index] = h;
        if (!h) p->unknown_logged[index] = 1;
    }
    if (!h) {
        /* the game goes on with 0, success for most of the system's functions, and with the offline console's error
           for the network's (net.c), logged the first time */
        uint32_t error = vita_net_offline_error(import->library);
        if (p->unknown_logged[index] == 1 || g_settings.vita_import_trace) {
            printf("vita: %s's 0x%08" PRIx32 " is not implemented (thread \"%s\", lr 0x%08" PRIx32 ")", import->library,
                   import->nid, t->name, t->cpu.r[14]);
            if (error) printf(": offline, 0x%08" PRIx32, error);
            putchar('\n');
            /* the registers the call has, and the code before it (halfwords to decode, the call's arguments are
               set up there), as a name for the import and its signature are found from them */
            printf("  args r0-r3 %08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 "; code before the call:",
                   t->cpu.r[0], t->cpu.r[1], t->cpu.r[2], t->cpu.r[3]);
            uint32_t from = (t->cpu.r[14] & ~1u) - 0x30;
            for (uint32_t o = 0; o < 0x30 && vita_mapped(p, from + o, 2); o += 2) printf(" %04" PRIx32, vita_read16(p, from + o));
            putchar('\n');
            p->unknown_logged[index] = 2;
        }
        t->cpu.r[0] = error;
        return;
    }
    t->call.redirected = false;
    uint32_t result = h->function(p, t);
    if (t->state == VITA_THREAD_READY && p->current == t && !t->call.redirected) t->cpu.r[0] = result;
    if (g_settings.vita_import_trace > 1 && (t->state == VITA_THREAD_WAITING || !trace_quiet(h->name)))
        printf("vita: \"%s\" %s(0x%" PRIx32 ", 0x%" PRIx32 ", 0x%" PRIx32 ", 0x%" PRIx32 ")%s 0x%08" PRIx32
               " (sp 0x%08" PRIx32 ", lr 0x%08" PRIx32 ")\n",
               t->name, h->name, r[0], r[1], r[2], r[3], t->state == VITA_THREAD_WAITING ? " waits," : " ->",
               t->cpu.r[0], t->cpu.r[13], t->cpu.r[14]);
}
void vita_hle_variables(VitaProcess* p) {
    for (uint32_t i = 0; i < p->image.variable_count; ++i) {
        const VitaImport* v = &p->image.variables[i];
        /* the two system variables no loaded guest module exports (see the NID comments at the top of this file) */
        if (v->nid == NID_SCE_LIBCPARAM) {
            /* Vita3K returns process_param->sce_libc_param.address(), or 0 when the game has no process param.
               SceProcessParam's layout (psdevwiki, Vita3K):
                 0x00 size, 0x04 magic ("PSP2"), 0x08 version,
                 0x0C pSceUserMainThreadName, 0x10 pSceUserMainThreadStackSize, 0x14 pSceUserMainThreadPriority,
                 0x18 pSceUserLibcParam, 0x1C pSceUserProcessParam.
               pSceUserLibcParam is at 0x18, not 0x1C (reading 0x1C would put pSceUserProcessParam's value into a
               slot libc treats as a SceLibcParam*, which libc then dereferences as a heap-size/version field and
               fails on). The slot ends up 0 when the game leaves pSceUserLibcParam null (default libc settings),
               matching Vita3K's `process_param ? ... : 0`. */
            uint32_t libcparam = 0, pp = p->image.process_param;
            if (pp && vita_mapped(p, pp, 4)) {
                uint32_t size = vita_read32(p, pp);
                if (size >= 0x1C && vita_mapped(p, pp + 0x18, 4)) libcparam = vita_read32(p, pp + 0x18);
            }
            vita_write32(p, v->stub, libcparam);
            continue;
        }
        if (v->nid == NID_LIBPERF_FLAG_PTR) {
            /* Vita3K's ScePerf VAR_EXPORT allocates a 4-byte capture flag (zeroed) and stores its address. The
               imported variable holds that pointer, which the perf code dereferences to check whether profiling is
               active; since ScarletNX doesn't capture, the flag stays 0. A failed allocation leaves the slot 0, the
               same as Vita3K's fresh allocation (which never fails for 4 bytes) would. */
            uint32_t flag = vita_alloc(p, &p->user, 4, 4);
            if (flag) vita_write32(p, flag, 0);
            vita_write32(p, v->stub, flag);
            continue;
        }
        /* a variable one of the loaded guest modules (libc, libfios2, libsmart, libult) exports: the imported
           variable's slot gets that export's address */
        const VitaExport* found = NULL;
        for (uint32_t j = 0; j < p->image.export_count; ++j) {
            const VitaExport* e = &p->image.exports[j];
            if (e->variable && e->library_nid == v->library_nid && e->nid == v->nid) {
                found = e;
                break;
            }
        }
        if (found) {
            /* The game's code was relocated to the slot itself (image.c), so the slot is the variable's storage, not a
               pointer to it: a C++ vtable or type_info that libc exports must read the same from the slot. Its bytes
               are copied (up to the slot's size, and not past the end of its segment). The export's own storage keeps
               serving the library that exports it, so a variable the game writes diverges from the library's copy */
            uint32_t size = VITA_VARIABLE_SIZE;
            while (size && !vita_mapped(p, found->address, size)) size /= 2;
            if (size) memcpy(vita_ptr(p, v->stub), vita_ptr(p, found->address), size);
            if (g_settings.vita_import_trace)
                printf("vita: variable %s 0x%08" PRIx32 " in slot 0x%08" PRIx32 " copied from export 0x%08" PRIx32
                       " (%" PRIu32 " bytes, first word 0x%08" PRIx32 ")\n", v->library, v->nid, v->stub, found->address, size,
                       vita_read32(p, v->stub));
            else printf("vita: %s's variable 0x%08" PRIx32 " exports an unmapped address 0x%08" PRIx32 "\n", v->library,
                        v->nid, found->address);
        } else {
            /* no loaded module exports it, so the HLE libc's table may */
            const VitaHleVariable* h = vita_hle_libc_variables;
            while (h->name && h->nid != v->nid) ++h;
            if (h->name) {
                h->init(p, v->stub);
                if (g_settings.vita_import_trace)
                    printf("vita: variable %s 0x%08" PRIx32 " in slot 0x%08" PRIx32 " from the HLE table (first word 0x%08" PRIx32
                           ")\n", v->library, v->nid, v->stub, vita_read32(p, v->stub));
            }
            else printf("vita: %s's variable 0x%08" PRIx32 " is not implemented (left zero)\n", v->library, v->nid);
        }
    }
}
void vita_hle_guest_call(VitaProcess* p, VitaThread* t, uint32_t func, const uint32_t* args, unsigned count,
                         VitaHleNext next, void* context) {
    SnxArmCPU* c = &t->cpu;
    if (!t->call.active) {
        t->call.active = true;
        t->call.lr = c->r[14], t->call.sp = c->r[13];
    } else if (!t->call.in_callback) {
        if (t->call.depth == sizeof t->call.stack / sizeof *t->call.stack) {
            c->r[0] = SCE_KERNEL_ERROR_ERROR;
            return;
        }
        t->call.stack[t->call.depth++] = (VitaCallFrame){t->call.lr, t->call.sp, t->call.next, t->call.context};
        t->call.lr = c->r[14], t->call.sp = c->r[13];
    }
    /* the arguments past the fourth on a stack of its own below the import's, 8-byte aligned */
    unsigned extra = count > 4 ? count - 4 : 0;
    uint32_t sp = (t->call.sp - extra * 4) & ~7u;
    for (unsigned i = 0; i < extra; ++i) vita_write32(p, sp + i * 4, args[4 + i]);
    for (unsigned i = 0; i < 4; ++i) c->r[i] = i < count ? args[i] : 0;
    c->r[13] = sp;
    c->r[14] = VITA_CALL_RETURN;
    c->r[15] = func & ~1u;
    c->cpsr = (c->cpsr & ~SNX_ARM_T) | (func & 1 ? SNX_ARM_T : 0);
    t->call.next = next, t->call.context = context, t->call.redirected = true;
}
void vita_hle_call_returned(VitaProcess* p, VitaThread* t) {
    SnxArmCPU* c = &t->cpu;
    if (!t->call.active || !t->call.next) {
        vita_thread_end(p, t, (int32_t)c->r[0]);
        return;
    }
    t->call.redirected = false;
    t->call.in_callback = true;
    uint32_t result = t->call.next(p, t, t->call.context);
    t->call.in_callback = false;
    if (t->call.redirected || t->state == VITA_THREAD_DEAD) return;
    uint32_t lr = t->call.lr, sp = t->call.sp;
    if (t->call.depth) {
        VitaCallFrame previous = t->call.stack[--t->call.depth];
        t->call.active = true;
        t->call.lr = previous.lr, t->call.sp = previous.sp;
        t->call.next = previous.next, t->call.context = previous.context;
    } else {
        t->call.active = false, t->call.next = NULL;
    }
    c->r[0] = result;
    c->r[13] = sp;
    c->r[15] = lr & ~1u;
    c->cpsr = (c->cpsr & ~SNX_ARM_T) | (lr & 1 ? SNX_ARM_T : 0);
}