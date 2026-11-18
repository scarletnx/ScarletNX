/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* GDB's JIT interface (gdb_jit.h), as GDB's manual ("JIT Compilation Interface") defines it: a descriptor GDB finds by
   name, holding a list of in-memory object files, and an empty function GDB breaks on, called after each change with
   the entry that changed. An object is a load's relocatable object with the loaded sections' sh_addr set to where the
   load put them (code_loader.c writes them into the copy), so GDB's reader takes their symbols at those addresses (an
   ET_REL symbol is its section's address plus st_value) and applies the object's relocations to .eh_frame itself */
#include "common/cpu/recompiler/llvm/gdb_jit.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

/* The names, layout and version are GDB's, which reads them from the process's memory */
enum { JIT_NOACTION, JIT_REGISTER_FN, JIT_UNREGISTER_FN };
struct jit_code_entry {
    struct jit_code_entry* next_entry;
    struct jit_code_entry* prev_entry;
    const char* symfile_addr;
    uint64_t symfile_size;
};
struct jit_descriptor {
    uint32_t version;
    uint32_t action_flag;
    struct jit_code_entry* relevant_entry;
    struct jit_code_entry* first_entry;
};
#ifdef _WIN32
/* the GDB stub is UNIX only (nx/runtime/gdbstub.c), and the loader's objects are COFF here: nothing is registered */
void snx_gdb_jit_enable(void) {}
unsigned char* snx_gdb_jit_copy(const void* owner, const void* object, size_t size) {
    (void)owner, (void)object, (void)size;
    return NULL;
}
void snx_gdb_jit_add(unsigned char* copy) { (void)copy; }
void snx_gdb_jit_remove(const void* owner) { (void)owner; }
#else
#include <pthread.h>

/* Weak, so a program that also links a strong definition (a static LLVM's GDB listener, ExecutionEngine's) builds, the
   descriptor then being that one. A shared libLLVM defines both too, and its references to the descriptor bind to the
   executable's (exported for GDB: src/nx/CMakeLists.txt). Either way another JIT in the process may link its entries
   into the same list, under its own lock: this file keeps its entries on a list of its own as well and only ever
   unlinks those. The breakpoint's call must stay, out of line, for GDB to stop at, and the clobber keeps the
   descriptor's stores before it */
void __jit_debug_register_code(void);
extern struct jit_descriptor __jit_debug_descriptor;
[[gnu::weak, gnu::noinline]] void __jit_debug_register_code(void) { __asm__ __volatile__("" ::: "memory"); }
[[gnu::weak]] struct jit_descriptor __jit_debug_descriptor = {1, JIT_NOACTION, NULL, NULL};

/* An entry and its object, one allocation: GDB reads the jit_code_entry at its start */
typedef struct Entry {
    struct jit_code_entry entry;
    const void* owner;
    struct Entry* next; /* this file's entries, newest first */
} Entry;

static struct {
    pthread_mutex_t lock; /* loaders load on different threads, and the lists are the process's */
    Entry* first;
    /* 0 until snx_gdb_jit_enable, then 1 for good, read without the lock: a loader that saw it on at a load sees it on
       at its free (it is stored once, and reads of one object keep their order), and one that saw it off registered
       nothing. Never on, nothing takes the lock (nor finds it held by a thread a fork left behind) */
    _Atomic int on;
} g_gdb_jit = {PTHREAD_MUTEX_INITIALIZER, NULL, 0};

void snx_gdb_jit_enable(void) { atomic_store_explicit(&g_gdb_jit.on, 1, memory_order_relaxed); }

/* under the lock */
static void notify(struct jit_code_entry* entry, uint32_t action) {
    __jit_debug_descriptor.relevant_entry = entry;
    __jit_debug_descriptor.action_flag = action;
    __jit_debug_register_code();
    __jit_debug_descriptor.relevant_entry = NULL;
    __jit_debug_descriptor.action_flag = JIT_NOACTION;
}

unsigned char* snx_gdb_jit_copy(const void* owner, const void* object, size_t size) {
    if (!atomic_load_explicit(&g_gdb_jit.on, memory_order_relaxed) || size > SIZE_MAX - sizeof(Entry)) return NULL;
    Entry* e = (Entry*)malloc(sizeof(Entry) + size);
    if (!e) return NULL;
    memcpy(e + 1, object, size);
    e->entry.symfile_addr = (const char*)(e + 1);
    e->entry.symfile_size = size;
    e->owner = owner;
    return (unsigned char*)(e + 1);
}

void snx_gdb_jit_add(unsigned char* copy) {
    Entry* e = (Entry*)(void*)(copy - sizeof(Entry));
    pthread_mutex_lock(&g_gdb_jit.lock);
    e->next = g_gdb_jit.first;
    g_gdb_jit.first = e;
    e->entry.prev_entry = NULL;
    e->entry.next_entry = __jit_debug_descriptor.first_entry;
    if (e->entry.next_entry) e->entry.next_entry->prev_entry = &e->entry;
    __jit_debug_descriptor.first_entry = &e->entry;
    notify(&e->entry, JIT_REGISTER_FN);
    pthread_mutex_unlock(&g_gdb_jit.lock);
}

void snx_gdb_jit_remove(const void* owner) {
    Entry* e;
    if (!atomic_load_explicit(&g_gdb_jit.on, memory_order_relaxed)) return;
    pthread_mutex_lock(&g_gdb_jit.lock);
    for (Entry** link = &g_gdb_jit.first; (e = *link) != NULL;) {
        if (e->owner != owner) {
            link = &e->next;
            continue;
        }
        *link = e->next;
        if (e->entry.prev_entry)
            e->entry.prev_entry->next_entry = e->entry.next_entry;
        else
            __jit_debug_descriptor.first_entry = e->entry.next_entry;
        if (e->entry.next_entry) e->entry.next_entry->prev_entry = e->entry.prev_entry;
        notify(&e->entry, JIT_UNREGISTER_FN);
        free(e);
    }
    pthread_mutex_unlock(&g_gdb_jit.lock);
}
#endif
