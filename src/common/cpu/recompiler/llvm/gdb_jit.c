/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* GDB's JIT interface (gdb_jit.h), as GDB's manual ("JIT Compilation Interface") defines it: a descriptor GDB finds by
   name, holding a list of in-memory object files, and an empty function GDB breaks on, called after each change with
   the entry that changed. An object is a load's relocatable object with the loaded sections' sh_addr set to where the
   load put them (code_loader.c writes them into the copy), so GDB's reader takes their symbols at those addresses (an
   ET_REL symbol is its section's address plus st_value) and applies the object's relocations to .eh_frame itself. On
   Linux the same objects also name their code for perf: one line per load in /tmp/perf-<pid>.map, with the address and
   size of the load's executable section */
/* O_NOFOLLOW and O_CLOEXEC need this before any system header: the snx_llvm target builds this file with strict
   -std=c23, which hides those constants otherwise */
#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include "common/cpu/recompiler/llvm/gdb_jit.h"
#include <stdint.h>
#include <stdio.h>
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
/* perf's map is Linux only */
void snx_gdb_jit_perf_map(int on) { (void)on; }
void snx_gdb_jit_perf_map_snapshot(void) {}
#else
#include <pthread.h>
#if defined(__linux__)
#include <elf.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

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

/* A completed perf map line, kept from enable until the map is enabled again or the process ends: a load can be freed
   while perf still holds samples that its code took. A snapshot writes these lines again at exit, so no earlier
   sample loses its name */
typedef struct PerfLine {
    struct PerfLine* next;
    int emitted; /* 1 once a load has written the line; a snapshot must not write it again */
    char text[96];
} PerfLine;

static struct {
    pthread_mutex_t lock; /* loaders load on different threads, and the lists are the process's */
    Entry* first;
    /* 0 until snx_gdb_jit_enable, then 1 for good, read without the lock: a loader that saw it on at a load sees it on
       at its free (it is stored once, and reads of one object keep their order), and one that saw it off registered
       nothing. Never on, nothing takes the lock (nor finds it held by a thread a fork left behind) */
    _Atomic int on;
    /* the perf map's gate, set once before the first load like on: names the same objects for perf as well as GDB */
    _Atomic int perf;
    /* 1 for good once snx_gdb_jit_add took an entry, stored by the thread that adds it: a loader's free comes after its
       own load, so it sees the 1 whenever it has an entry, and with none ever added it takes no lock */
    _Atomic int seen;
    /* the file's own lock: no file I/O runs under lock, but one writer at a time keeps lines from interleaving and keeps
       a snapshot's rewrite from racing a load's append */
    pthread_mutex_t perf_io_lock;
    PerfLine* perf_first; /* this file's lines, under lock */
} g_gdb_jit = {PTHREAD_MUTEX_INITIALIZER, NULL, 0, 0, 0, PTHREAD_MUTEX_INITIALIZER, NULL};

void snx_gdb_jit_enable(void) { atomic_store_explicit(&g_gdb_jit.on, 1, memory_order_relaxed); }

#if defined(__linux__)
/* perf reads /tmp/perf-<pid>.map to name the code the runtime makes: one line per load, the loaded .text's address and
   size in hex without 0x, and a name for the batch. The object is a copy whose sh_addr the loader set where it put the
   code (gdb_jit.c's header), so its executable section says where the line's range is. The path is fixed, so open it
   without following a link and only when it names a regular file: a link or a special file in /tmp must not make this
   writer or perf's reader touch another object */
static FILE* perf_map_open(int append) {
    char path[64];
    if (snprintf(path, sizeof path, "/tmp/perf-%ld.map", (long)getpid()) >= (int)sizeof path) return NULL;
    int fd = open(path, O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK | (append ? O_APPEND : O_TRUNC), 0666);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) {
        close(fd);
        return NULL;
    }
    FILE* f = fdopen(fd, append ? "a" : "w");
    if (!f) close(fd);
    return f;
}

/* formats one line into text; 0 when the object is not an ELF64 the parser can read */
static int perf_map_text(const void* object, size_t size, char* text, size_t cap) {
    const unsigned char* o = (const unsigned char*)object;
    Elf64_Ehdr eh;
    Elf64_Shdr sh;
    uint64_t start = 0, length = 0;
    if (size < sizeof eh) return 0;
    memcpy(&eh, o, sizeof eh);
    /* the fields below are read without a byte swap, so the object must be a little-endian ELF64 with the header and
       section table sizes this parser expects */
    if (memcmp(eh.e_ident, ELFMAG, SELFMAG) || eh.e_ident[EI_CLASS] != ELFCLASS64 ||
        eh.e_ident[EI_DATA] != ELFDATA2LSB || eh.e_ehsize != sizeof eh || eh.e_shentsize != sizeof sh ||
        !eh.e_shnum || eh.e_shoff < eh.e_ehsize || eh.e_shoff > size ||
        (size - eh.e_shoff) / sizeof sh < eh.e_shnum)
        return 0;
    for (size_t i = 0; i < eh.e_shnum; ++i) {
        memcpy(&sh, o + eh.e_shoff + i * sizeof sh, sizeof sh);
        if (!(sh.sh_flags & SHF_EXECINSTR) || !sh.sh_addr || !sh.sh_size) continue;
        start = sh.sh_addr;
        length = sh.sh_size;
        break;
    }
    if (!length) return 0;
    int len = snprintf(text, cap, "%llx %llx jit_%llx\n", (unsigned long long)start, (unsigned long long)length,
                       (unsigned long long)start);
    return len > 0 && (size_t)len < cap;
}

/* under lock: forget the lines of an earlier session, matching the file's truncation at enable */
static void perf_map_clear(void) {
    PerfLine* line = g_gdb_jit.perf_first;
    while (line) {
        PerfLine* next = line->next;
        free(line);
        line = next;
    }
    g_gdb_jit.perf_first = NULL;
}

/* under lock: keep the line for a snapshot, so a load that goes still has its name in the map */
static void perf_map_record(const char* text) {
    PerfLine* line = (PerfLine*)malloc(sizeof *line);
    if (!line) return;
    snprintf(line->text, sizeof line->text, "%s", text);
    line->emitted = 0;
    line->next = g_gdb_jit.perf_first;
    g_gdb_jit.perf_first = line;
}

/* perf_io_lock is held by the caller, either here or in snx_gdb_jit_add's emit-and-mark block. */
static void perf_map_emit_locked(const char* text) {
    FILE* f = perf_map_open(1);
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}
/* at enable: remove the lines a crash or a reused pid left in the map */
static void perf_map_truncate(void) {
    pthread_mutex_lock(&g_gdb_jit.perf_io_lock);
    FILE* f = perf_map_open(0);
    if (f) fclose(f);
    pthread_mutex_unlock(&g_gdb_jit.perf_io_lock);
}

/* at exit: write the lines a load recorded but has not emitted itself, so every earlier sample keeps its name.
   perf_io_lock serializes the writes; a line already emitted is skipped to avoid a duplicate symbol. */
static void perf_map_snapshot(void) {
    /* Append only the lines a load has not emitted itself. Opening the map for append keeps the lines already
       written; a truncate here would erase them when there is nothing new to add. No buffer is needed, so there is
       no allocation failure to lose a name to and no uninitialized byte to print. */
    pthread_mutex_lock(&g_gdb_jit.perf_io_lock);
    pthread_mutex_lock(&g_gdb_jit.lock);
    for (PerfLine* line = g_gdb_jit.perf_first; line; line = line->next) {
        if (line->emitted) continue;
        FILE* f = perf_map_open(1);
        if (f) {
            fputs(line->text, f);
            fclose(f);
        }
        line->emitted = 1;
    }
    pthread_mutex_unlock(&g_gdb_jit.lock);
    pthread_mutex_unlock(&g_gdb_jit.perf_io_lock);
}
#else
static int perf_map_text(const void* object, size_t size, char* text, size_t cap) {
    (void)object, (void)size, (void)text, (void)cap;
    return 0;
}
static void perf_map_clear(void) {}
static void perf_map_record(const char* text) { (void)text; }
static void perf_map_emit_locked(const char* text) { (void)text; }
static void perf_map_truncate(void) {}
static void perf_map_snapshot(void) {}
#endif

void snx_gdb_jit_perf_map(int on) {
    if (on && !atomic_load_explicit(&g_gdb_jit.perf, memory_order_relaxed)) {
        perf_map_truncate();
        pthread_mutex_lock(&g_gdb_jit.lock);
        perf_map_clear();
        pthread_mutex_unlock(&g_gdb_jit.lock);
    }
    atomic_store_explicit(&g_gdb_jit.perf, on ? 1 : 0, memory_order_relaxed);
}

void snx_gdb_jit_perf_map_snapshot(void) {
    if (!atomic_load_explicit(&g_gdb_jit.perf, memory_order_relaxed)) return;
    perf_map_snapshot();
}

/* under the lock */
static void notify(struct jit_code_entry* entry, uint32_t action) {
    __jit_debug_descriptor.relevant_entry = entry;
    __jit_debug_descriptor.action_flag = action;
    __jit_debug_register_code();
    __jit_debug_descriptor.relevant_entry = NULL;
    __jit_debug_descriptor.action_flag = JIT_NOACTION;
}

unsigned char* snx_gdb_jit_copy(const void* owner, const void* object, size_t size) {
    int gdb = atomic_load_explicit(&g_gdb_jit.on, memory_order_relaxed);
    int perf = atomic_load_explicit(&g_gdb_jit.perf, memory_order_relaxed);
    if ((!gdb && !perf) || size > SIZE_MAX - sizeof(Entry)) return NULL;
    Entry* e = (Entry*)malloc(sizeof(Entry) + size);
    if (!e) return NULL;
    memcpy(e + 1, object, size);
    e->entry.symfile_addr = (const char*)(e + 1);
    e->entry.symfile_size = size;
    /* An entry made in perf-only mode has no GDB list links. This relies on "enable before the first load": if GDB
       was on when the entry was added, snx_gdb_jit_add linked them; otherwise the entry is never unlinked. */
    e->entry.prev_entry = NULL;
    e->entry.next_entry = NULL;
    e->next = NULL;
    e->owner = owner;
    return (unsigned char*)(e + 1);
}

void snx_gdb_jit_add(unsigned char* copy) {
    Entry* e = (Entry*)(void*)(copy - sizeof(Entry));
    char text[96] = {0}; /* the line's text, made under lock and written after it */
    const char* pending = NULL;
    int perf = atomic_load_explicit(&g_gdb_jit.perf, memory_order_relaxed);
    atomic_store_explicit(&g_gdb_jit.seen, 1, memory_order_relaxed);
    pthread_mutex_lock(&g_gdb_jit.lock);
    e->next = g_gdb_jit.first;
    g_gdb_jit.first = e;
    /* only GDB's own entries go on its list: perf's map is written without touching the descriptor */
    if (atomic_load_explicit(&g_gdb_jit.on, memory_order_relaxed)) {
        e->entry.prev_entry = NULL;
        e->entry.next_entry = __jit_debug_descriptor.first_entry;
        if (e->entry.next_entry) e->entry.next_entry->prev_entry = &e->entry;
        __jit_debug_descriptor.first_entry = &e->entry;
        notify(&e->entry, JIT_REGISTER_FN);
    }
    if (perf && perf_map_text(e->entry.symfile_addr, (size_t)e->entry.symfile_size, text, sizeof text)) {
        perf_map_record(text);
        pending = text;
    }
    pthread_mutex_unlock(&g_gdb_jit.lock);
    if (pending) {
        /* Take the file lock first, then mark the line under the line lock while still holding it: this is the same
           order the exit snapshot uses, so it cannot append a line that is about to be marked as emitted. */
        pthread_mutex_lock(&g_gdb_jit.perf_io_lock);
        perf_map_emit_locked(pending);
        pthread_mutex_lock(&g_gdb_jit.lock);
        for (PerfLine* line = g_gdb_jit.perf_first; line; line = line->next)
            if (!strcmp(line->text, pending)) line->emitted = 1;
        pthread_mutex_unlock(&g_gdb_jit.lock);
        pthread_mutex_unlock(&g_gdb_jit.perf_io_lock);
    }
}

void snx_gdb_jit_remove(const void* owner) {
    Entry* e;
    if (!atomic_load_explicit(&g_gdb_jit.seen, memory_order_relaxed)) return;
    int gdb = atomic_load_explicit(&g_gdb_jit.on, memory_order_relaxed);
    pthread_mutex_lock(&g_gdb_jit.lock);
    for (Entry** link = &g_gdb_jit.first; (e = *link) != NULL;) {
        if (e->owner != owner) {
            link = &e->next;
            continue;
        }
        *link = e->next;
        if (gdb) {
            if (e->entry.prev_entry)
                e->entry.prev_entry->next_entry = e->entry.next_entry;
            else
                __jit_debug_descriptor.first_entry = e->entry.next_entry;
            if (e->entry.next_entry) e->entry.next_entry->prev_entry = e->entry.prev_entry;
            notify(&e->entry, JIT_UNREGISTER_FN);
        }
        free(e);
    }
    pthread_mutex_unlock(&g_gdb_jit.lock);
}
#endif
