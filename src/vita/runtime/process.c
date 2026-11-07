/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#if !defined(_WIN32) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE /* MAP_ANONYMOUS, MAP_NORESERVE */
#endif
#include "vita/runtime/process.h"
#include "vita/runtime/watch.h"
#include "vita/hle/hle.h"
#include "vita/loader/vpk.h"
#include "common/utilities/common.h"
#include "common/utilities/endian.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <inttypes.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif

#define PAGE 0x1000u
#define MAIN_STACK 0x40000u /* the kernel's default when the process parameters name none */
#define MAIN_PRIORITY 0x10000100u /* SCE_KERNEL_DEFAULT_PRIORITY_USER */

uint64_t vita_now_ns(void) {
#ifdef _WIN32
    return (uint64_t)(snx_seconds() * 1e9);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
#endif
}
uint16_t vita_read16(VitaProcess* p, uint32_t address) { return snx_ld_le16(p->mem + address); }
uint32_t vita_read32(VitaProcess* p, uint32_t address) { return snx_ld_le32(p->mem + address); }
void vita_write16(VitaProcess* p, uint32_t address, uint16_t value) { snx_st_le16(p->mem + address, value); }
void vita_write32(VitaProcess* p, uint32_t address, uint32_t value) { snx_st_le32(p->mem + address, value); }
void vita_write64(VitaProcess* p, uint32_t address, uint64_t value) { snx_st_le64(p->mem + address, value); }
bool vita_string(VitaProcess* p, uint32_t address, char* out, size_t size) {
    if (!size) return false;
    out[0] = 0;
    if (!address) return false;
    size_t i = 0;
    for (; i + 1 < size && p->mem[(uint32_t)(address + i)]; ++i) out[i] = (char)p->mem[(uint32_t)(address + i)];
    out[i] = 0;
    return true;
}

/* ---- memory: pages made accessible (zero) and taken back ---- */
static bool commit(VitaProcess* p, uint32_t address, uint32_t size) {
    vita_watch_release(address, size);
    return !snx_vm_protect(p->mem + address, size, SNX_VM_READ_WRITE);
}
/* inaccessible again, and zero when committed next */
static void decommit(VitaProcess* p, uint32_t address, uint32_t size) {
    vita_watch_release(address, size);
#ifdef _WIN32
    (void)snx_vm_protect(p->mem + address, size, SNX_VM_NONE);
#else
    (void)mmap(p->mem + address, size, PROT_NONE, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
#endif
}
bool vita_mapped(VitaProcess* p, uint32_t address, uint32_t size) {
    uint64_t end = (uint64_t)address + size;
    if (end > VITA_SPACE) return false;
    for (unsigned i = 0; i < p->image.segment_count; ++i) {
        const VitaSegment* s = &p->image.segment[i];
        if (address >= s->address && end <= s->address + (uint64_t)((s->size + PAGE - 1) & ~(PAGE - 1))) return true;
    }
    if (address >= VITA_RUNTIME_PAGE && end <= VITA_TLS_BASE + (uint64_t)VITA_THREADS * VITA_TLS_SIZE) return true;
    /* The imported variables' slots: only the used ones, so an address past them stays unmapped and a stray read
       faults. VITA_VARIABLE_COUNT is the most the region can hold, and p->image.variable_count is how many are used. */
    if (p->image.variable_count && address >= VITA_VARIABLES &&
        end <= VITA_VARIABLES + (uint64_t)p->image.variable_count * VITA_VARIABLE_SIZE)
        return true;
    for (unsigned i = 0; i < VITA_ALLOCATIONS; ++i) {
        const VitaAllocation* a = &p->allocations[i];
        if (a->size && address >= a->address && end <= (uint64_t)a->address + a->size) return true;
    }
    return false;
}
uint32_t vita_alloc(VitaProcess* p, VitaRegion* region, uint32_t size, uint32_t align) {
    if (!size) return 0;
    size = (size + PAGE - 1) & ~(PAGE - 1);
    if (align < PAGE) align = PAGE;
    unsigned slot = VITA_ALLOCATIONS;
    for (unsigned i = 0; i < VITA_ALLOCATIONS && slot == VITA_ALLOCATIONS; ++i)
        if (!p->allocations[i].size) slot = i;
    if (slot == VITA_ALLOCATIONS) return 0;
    /* first fit from the lowest address that may be free: each candidate moved past the allocations it overlaps */
    uint64_t at = ((uint64_t)region->next + align - 1) & ~(uint64_t)(align - 1);
    for (bool moved = true; moved && at + size <= region->end;) {
        moved = false;
        for (unsigned i = 0; i < VITA_ALLOCATIONS; ++i) {
            const VitaAllocation* a = &p->allocations[i];
            if (a->size && at < (uint64_t)a->address + a->size && a->address < at + size) {
                at = ((uint64_t)a->address + a->size + align - 1) & ~(uint64_t)(align - 1);
                moved = true;
            }
        }
    }
    if (at + size > region->end || !commit(p, (uint32_t)at, size)) return 0;
    p->allocations[slot] = (VitaAllocation){(uint32_t)at, size};
    if (region->next == at) region->next = (uint32_t)(at + size);
    return (uint32_t)at;
}
void vita_free(VitaProcess* p, uint32_t address) {
    for (unsigned i = 0; i < VITA_ALLOCATIONS; ++i) {
        VitaAllocation* a = &p->allocations[i];
        if (!a->size || a->address != address) continue;
        decommit(p, a->address, a->size);
        VitaRegion* r = address >= p->cdram.base && address < p->cdram.end ? &p->cdram : &p->user;
        if (address < r->next) r->next = address;
        *a = (VitaAllocation){0};
        return;
    }
}

/* ---- objects ---- */
#define UID_BASE 0x40010001u
uint32_t vita_object_new(VitaProcess* p, uint32_t type, const char* name, VitaObject** out) {
    for (uint32_t i = 0; i < VITA_OBJECTS; ++i) {
        VitaObject* o = &p->objects[i];
        if (o->type != VITA_OBJ_FREE) continue;
        memset(o, 0, sizeof *o);
        o->type = type;
        snprintf(o->name, sizeof o->name, "%s", name ? name : "");
        if (out) *out = o;
        return UID_BASE + i * 2;
    }
    return 0;
}
VitaObject* vita_object(VitaProcess* p, uint32_t uid, uint32_t type) {
    uint32_t i = (uid - UID_BASE) / 2;
    if (uid < UID_BASE || (uid - UID_BASE) & 1 || i >= VITA_OBJECTS) return NULL;
    VitaObject* o = &p->objects[i];
    return o->type == type ? o : NULL;
}
void vita_object_delete(VitaProcess* p, uint32_t uid) {
    uint32_t i = (uid - UID_BASE) / 2;
    if (uid >= UID_BASE && i < VITA_OBJECTS) memset(&p->objects[i], 0, sizeof p->objects[i]);
}

/* ---- threads ---- */
int32_t vita_thread_new(VitaProcess* p, const char* name, uint32_t entry, uint32_t priority, uint32_t stack_size,
                        uint32_t attr) {
    unsigned index = VITA_THREADS;
    for (unsigned i = 0; i < VITA_THREADS && index == VITA_THREADS; ++i)
        if (p->threads[i].state == VITA_THREAD_FREE) index = i;
    if (index == VITA_THREADS) return (int32_t)SCE_KERNEL_ERROR_NO_MEMORY;
    if (stack_size < 0x1000) stack_size = 0x1000;
    uint32_t stack = vita_alloc(p, &p->user, stack_size, PAGE);
    VitaObject* o;
    uint32_t uid = stack ? vita_object_new(p, VITA_OBJ_THREAD, name, &o) : 0;
    if (!uid) {
        if (stack) vita_free(p, stack);
        return (int32_t)SCE_KERNEL_ERROR_NO_MEMORY;
    }
    o->u.thread = index;
    VitaThread* t = &p->threads[index];
    memset(t, 0, sizeof *t);
    t->state = VITA_THREAD_DORMANT;
    t->uid = uid;
    snprintf(t->name, sizeof t->name, "%s", name ? name : "");
    t->entry = entry, t->priority = priority, t->attr = attr;
    t->stack = stack, t->stack_size = (stack_size + PAGE - 1) & ~(PAGE - 1);
    t->tls = VITA_TLS_BASE + index * VITA_TLS_SIZE;
    memset(p->mem + t->tls, 0, VITA_TLS_SIZE);
    return (int32_t)uid;
}
VitaThread* vita_thread(VitaProcess* p, uint32_t uid) {
    VitaObject* o = vita_object(p, uid, VITA_OBJ_THREAD);
    return o ? &p->threads[o->u.thread] : NULL;
}
void vita_thread_start(VitaProcess* p, VitaThread* t, uint32_t r0, uint32_t r1) {
    SnxArmCPU* cpu = &t->cpu;
    memset(cpu, 0, sizeof *cpu);
    cpu->mem = p->mem;
    cpu->mem_size = VITA_SPACE;
    cpu->cpsr = 0x10 | (t->entry & 1 ? SNX_ARM_T : 0);
    cpu->r[15] = t->entry & ~1u;
    cpu->r[0] = r0, cpu->r[1] = r1;
    /* the stack's top, below the argument block the caller may have copied there */
    cpu->r[13] = r1 && r1 >= t->stack && r1 < t->stack + t->stack_size ? r1 & ~7u : t->stack + t->stack_size;
    cpu->r[14] = VITA_THREAD_RETURN;
    cpu->tpidruro = t->tls + VITA_TLS_SIZE / 2;
    t->state = VITA_THREAD_READY;
    t->seq = ++p->seq;
}
void vita_thread_end(VitaProcess* p, VitaThread* t, int32_t status) {
    /* a thread's status is its entry function's return value. For the main thread this is the last module_start's
       result (libc, then libfios2, then libsmart, then libult, then the game's own): a non-zero value here is the
       module that aborted the chain, and the number is the module's own error code */
    if (t->name[0]) printf("vita: thread \"%s\" ended with status 0x%08x\n", t->name, (unsigned)status);
    t->exit_status = status;
    t->state = VITA_THREAD_DEAD;
    if (p->current == t) p->current = NULL;
}
uint64_t vita_deadline(VitaProcess* p, uint32_t timeout) {
    if (!timeout) return 0;
    uint64_t deadline = vita_now_ns() + (uint64_t)vita_read32(p, timeout) * 1000u;
    return deadline ? deadline : 1;
}
void vita_wait(VitaProcess* p, VitaWait wait) {
    VitaThread* t = p->current;
    if (!t) return;
    t->wait = wait;
    t->state = VITA_THREAD_WAITING;
    p->current = NULL;
}

/* ---- the process ---- */
static bool trampolines(VitaProcess* p) {
    /* A32: a thread's entry returning (its lr); the display queue's thread, which takes each entry and calls its
       callback with the entry's data, forever */
    static const uint32_t code[] = {
        0xef000000u | VITA_SVC_THREAD_RETURN, /* 0x00: svc THREAD_RETURN */
        0xeafffffeu,                          /* b . */
        0xe320f000u, 0xe320f000u,             /* nop, nop */
        0xef000000u | VITA_SVC_DISPLAY_NEXT,  /* 0x10: svc DISPLAY_NEXT -> r0 the data, r1 the callback */
        0xe12fff31u,                          /* blx r1 */
        0xef000000u | VITA_SVC_DISPLAY_DONE,  /* svc DISPLAY_DONE */
        0xeafffffbu,                          /* b 0x10 */
        0xe12fff1eu,                          /* 0x20: bx lr */
        0xef000000u | VITA_SVC_FIBER_RETURN,  /* 0x24: svc FIBER_RETURN */
        0xeafffffeu,                          /* b . */
        0xef000000u | VITA_SVC_CALL_RETURN,   /* 0x2c: svc CALL_RETURN */
        0xeafffffeu,                          /* b . */
    };
    if (!commit(p, VITA_RUNTIME_PAGE, VITA_TLS_BASE + VITA_THREADS * VITA_TLS_SIZE - VITA_RUNTIME_PAGE)) return false;
    for (size_t i = 0; i < sizeof code / sizeof *code; ++i) vita_write32(p, VITA_RUNTIME_PAGE + (uint32_t)i * 4, code[i]);
    return true;
}
/* the game's title ID, from its param.sfo (the update's when it has one), else the folder's name */
static void title_id(VitaProcess* p) {
    char* title = NULL;
    for (int i = 0; i < 2 && !title; ++i) {
        char* path = snx_path_join(i ? p->app : p->patch, "sce_sys/param.sfo");
        size_t size = 0;
        unsigned char* sfo = path ? snx_read_file(path, &size) : NULL;
        title = sfo ? vita_sfo_string(sfo, size, "TITLE_ID") : NULL;
        if (title && !vita_title_id(title)) free(title), title = NULL;
        free(sfo);
        free(path);
    }
    const char* name = title;
#ifdef _WIN32
    const char* separators = "/\\"; /* Windows' absolute paths have backslashes */
#else
    const char* separators = "/";
#endif
    if (!name) /* the folder's last name */
        for (const char* c = name = p->folder; *c; ++c)
            if (strchr(separators, *c)) name = c + 1;
    snprintf(p->title_id, sizeof p->title_id, "%s", name);
    free(title);
}
static uint32_t vita_module_start_next(VitaProcess* p, VitaThread* t, void* context) {
    uintptr_t index = (uintptr_t)context;
    /* The chain of module_start functions: each library's in turn, then the game's. The thread's entry is the
       first, and a result other than SCE_OK from any of them ends the chain. */
    printf("vita: module_start_next index=%u previous-r0=0x%08" PRIx32 "\n", (unsigned)index, t->cpu.r[0]);
    if (t->cpu.r[0] != SCE_OK) return t->cpu.r[0];
    if (index < p->image.library_start_count) {
        uint32_t entry = p->image.library_starts[index];
        vita_hle_guest_call(p, t, entry, NULL, 0, vita_module_start_next, (void*)(index + 1));
        return 0;
    }
    if (index == p->image.library_start_count) {
        vita_hle_guest_call(p, t, p->image.module_start, NULL, 0, vita_module_start_next,
                            (void*)(index + 1));
        return 0;
    }
    return t->cpu.r[0];
}
/* whether t is running a module_start of the chain. A module_start may end with sceKernelExitThread, which then
   counts as its return */
bool vita_in_module_start(const VitaThread* t) { return t->call.active && t->call.next == vita_module_start_next; }
VitaProcess* vita_process_new(const VitaImage* image, const char* folder, char* error, size_t error_size) {
    VitaProcess* p = calloc(1, sizeof *p);
    if (!p) {
        snprintf(error, error_size, "out of memory");
        return NULL;
    }
    p->image = *image;
    p->folder = snx_absolute(folder);
    p->app = p->folder ? snx_path_join(p->folder, "app") : NULL;
    p->ux0 = p->folder ? snx_path_join(p->folder, "ux0") : NULL;
    p->patch = p->folder ? snx_path_join(p->folder, "patch") : NULL;
    p->user = (VitaRegion){VITA_USER_BASE, VITA_USER_END, VITA_USER_BASE};
    p->cdram = (VitaRegion){VITA_CDRAM_BASE, VITA_CDRAM_END, VITA_CDRAM_BASE};
    p->start_ns = vita_now_ns();
    p->unknown_logged = calloc(image->import_count ? image->import_count : 1, sizeof *p->unknown_logged);
    p->hle = calloc(image->import_count ? image->import_count : 1, sizeof *p->hle);
    /* the guest's 4 GiB and a guard page past it, inaccessible until committed */
    p->mem = snx_vm_reserve(VITA_SPACE + 0x10000);
    if (!p->app || !p->ux0 || !p->patch || !p->unknown_logged || !p->hle || !p->mem || !trampolines(p)) {
        snprintf(error, error_size, "cannot reserve the guest's address space");
        goto fail;
    }
    if (snx_mkdirs(p->ux0)) {
        snprintf(error, error_size, "cannot create %s", p->ux0);
        goto fail;
    }
    title_id(p);
    p->savedata = snx_format("%s/user/00/savedata/%s", p->ux0, p->title_id);
    if (!p->savedata || snx_mkdirs(p->savedata)) {
        snprintf(error, error_size, "cannot create the game's save data folder in %s", p->ux0);
        goto fail;
    }
    for (unsigned i = 0; i < image->segment_count; ++i) {
        const VitaSegment* s = &image->segment[i];
        if (!commit(p, s->address, (s->size + PAGE - 1) & ~(PAGE - 1))) {
            snprintf(error, error_size, "cannot map the segment at 0x%08x", (unsigned)s->address);
            goto fail;
        }
        memcpy(p->mem + s->address, s->bytes, s->size);
    }
    /* The imported variables' slots (VitaImage.variables): one for each variable each module imports. Map the used
       range, and nothing past it, before vita_hle_variables writes through them. */
    if (image->variable_count) {
        uint32_t variables_size = (image->variable_count * VITA_VARIABLE_SIZE + PAGE - 1) & ~(PAGE - 1);
        if (!commit(p, VITA_VARIABLES, variables_size)) {
            snprintf(error, error_size, "cannot map the %u imported variables", image->variable_count);
            goto fail;
        }
    }
    vita_hle_variables(p);
    /* the main thread, at module_start, with the stack and priority the process parameters name (sceProcessParam's
       main_thread_priority and main_thread_stacksize, pointers when set) */
    uint32_t stack = MAIN_STACK, priority = MAIN_PRIORITY;
    if (image->process_param && vita_read32(p, image->process_param + 4) == 0x32505350u) { /* "PSP2" */
        uint32_t prio = vita_read32(p, image->process_param + 20), size = vita_read32(p, image->process_param + 24);
        if (prio && vita_mapped(p, prio, 4)) priority = vita_read32(p, prio);
        if (size && vita_mapped(p, size, 4)) stack = vita_read32(p, size);
    }
    uint32_t first_start = image->library_start_count ? image->library_starts[0] : image->module_start;
    int32_t uid = vita_thread_new(p, "main thread", first_start, priority, stack, 0);
    if (uid < 0) {
        snprintf(error, error_size, "cannot create the main thread");
        goto fail;
    }
    VitaThread* main_thread = vita_thread(p, (uint32_t)uid);
    vita_thread_start(p, main_thread, 0, 0);
    if (image->library_start_count) {
        main_thread->call.active = true;
        main_thread->call.lr = VITA_THREAD_RETURN;
        main_thread->call.sp = main_thread->cpu.r[13];
        main_thread->call.next = vita_module_start_next;
        main_thread->call.context = (void*)1;
        main_thread->cpu.r[14] = VITA_CALL_RETURN;
    }
    return p;
fail:
    vita_process_free(p);
    return NULL;
}
void vita_process_free(VitaProcess* p) {
    if (!p) return;
    vita_io_close_all(p);
    vita_audio_free(p);
    vita_mp4_free(p);
    vita_videodec_free(p);
    vita_audiodec_free(p);
    vita_ngs_free(p);
    vita_net_free(p);
    vita_trophy_free(p);
    vita_gxm_free(p);
    vita_watch_free();
    if (p->mem) snx_vm_release(p->mem, VITA_SPACE + 0x10000);
    free(p->unknown_logged);
    free(p->hle);
    free(p->folder);
    free(p->app);
    free(p->patch);
    free(p->ux0);
    free(p->savedata);
    free(p);
}