/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The guest's dynamic linker (linker.h). The formats are the ELF specification, its AArch64 supplement, and Android's
   packed relocations (DT_ANDROID_RELA, "APS2") as bionic's linker documents them */
#include "android/bridge/linker.h"
#include "android/bridge/bridge.h"
#include "android/bridge/hle.h"
#include "android/bridge/host.h"
#include "common/cpu/recompiler/llvm/elf_defs.h" /* the ELF names on every host, whose own may have none */
#include "common/cpu/recompiler/llvm/elf_hash.h"
#include "common/utilities/common.h"
#include "common/platform/file.h"
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <io.h>
#define LIBRARY_OPEN_FLAGS (O_RDONLY | O_BINARY | O_NOINHERIT)
#else
#include <unistd.h>
#define LIBRARY_OPEN_FLAGS (O_RDONLY | O_CLOEXEC)
#endif

#ifndef DT_ANDROID_RELA
#define DT_ANDROID_RELA   0x60000011
#define DT_ANDROID_RELASZ 0x60000012
#endif
#ifndef R_AARCH64_TLS_DTPMOD64
#define R_AARCH64_TLS_DTPMOD64 1028
#define R_AARCH64_TLS_DTPREL64 1029
#define R_AARCH64_TLSDESC      1031
#endif
#ifndef DT_RELR
#define DT_RELR   36
#define DT_RELRSZ 35
#endif

struct GuestLibrary {
    char* name; /* its soname, or its file's name */
    char* path;
    uint64_t base, size; /* the guest address of its first byte (vaddr 0) and its span */
    const Elf64_Dyn* dynamic;
    const Elf64_Sym* symbols;
    const char* strings;
    const uint32_t* hash;     /* DT_HASH */
    const uint32_t* gnu_hash; /* DT_GNU_HASH */
    uint32_t symbol_count;
    GuestLibrary** needed;
    size_t needed_count;
    GuestLibrary* next;
    bool symbolic;
    uint64_t phdr; /* its program headers, in guest memory */
    unsigned phnum;
    unsigned tls_module; /* its PT_TLS's module (1 on), 0 for none */
};

static struct {
    GuestLibrary* first; /* in load order: the global group */
    char* library_dir;
    pthread_mutex_t lock;
} g = {.lock = PTHREAD_MUTEX_INITIALIZER};

static char* copy(const char* s) {
    size_t n = strlen(s) + 1;
    char* c = snx_alloc(n);
    memcpy(c, s, n);
    return c;
}

static bool linker_verbose;
void linker_set_verbose(bool verbose) {
    linker_verbose = verbose;
}

void linker_set_library_dir(const char* dir) {
    free(g.library_dir);
    g.library_dir = copy(dir);
}

const char* linker_name(const GuestLibrary* library) {
    return library->name;
}

static const char* base_name(const char* path) {
    const char* slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

/* ---- symbols ---- */

/* a symbol the library defines, NULL for none */
static const Elf64_Sym* defined(const GuestLibrary* l, const char* name) {
    if (l->gnu_hash) {
        const uint32_t* h = l->gnu_hash;
        uint32_t buckets = h[0], offset = h[1], bloom_size = h[2], shift = h[3];
        const uint64_t* bloom = (const uint64_t*)(h + 4);
        const uint32_t* bucket = (const uint32_t*)(bloom + bloom_size);
        const uint32_t* chain = bucket + buckets;
        uint32_t hash = snx_elf_gnu_hash(name);
        uint64_t word = bloom[(hash / 64) % bloom_size];
        if (!((word >> (hash % 64)) & (word >> ((hash >> shift) % 64)) & 1)) return NULL;
        for (uint32_t i = bucket[hash % buckets]; i >= offset && i; ++i) {
            const Elf64_Sym* s = &l->symbols[i];
            uint32_t c = chain[i - offset];
            if ((c | 1) == (hash | 1) && !strcmp(l->strings + s->st_name, name) && s->st_shndx != SHN_UNDEF)
                return s;
            if (c & 1) break;
        }
        return NULL;
    }
    if (l->hash) {
        uint32_t buckets = l->hash[0];
        const uint32_t *bucket = l->hash + 2, *chain = bucket + buckets;
        for (uint32_t i = bucket[snx_elf_sysv_hash(name) % buckets]; i; i = chain[i]) {
            const Elf64_Sym* s = &l->symbols[i];
            if (!strcmp(l->strings + s->st_name, name) && s->st_shndx != SHN_UNDEF) return s;
        }
    }
    return NULL;
}

static uint64_t symbol_address(const GuestLibrary* l, const Elf64_Sym* s) {
    return l->base + s->st_value;
}

/* a library and its DT_NEEDED tree, breadth first, as dlsym of a handle searches */
static uint64_t search_tree(GuestLibrary* root, const char* name) {
    GuestLibrary* queue[256];
    size_t head = 0, tail = 0;
    queue[tail++] = root;
    while (head < tail) {
        GuestLibrary* l = queue[head++];
        const Elf64_Sym* s = defined(l, name);
        if (s) return symbol_address(l, s);
        for (size_t i = 0; i < l->needed_count && tail < 256; ++i) {
            bool seen = false;
            for (size_t k = 0; k < tail; ++k) seen |= queue[k] == l->needed[i];
            if (!seen && l->needed[i]) queue[tail++] = l->needed[i];
        }
    }
    return 0;
}

uint64_t linker_symbol(GuestLibrary* library, const char* name) {
    if (library) {
        uint64_t a = search_tree(library, name);
        return a ? a : hle_symbol(name);
    }
    for (GuestLibrary* l = g.first; l; l = l->next) {
        const Elf64_Sym* s = defined(l, name);
        if (s) return symbol_address(l, s);
    }
    return hle_symbol(name);
}

/* an import of a library resolved: the library itself first when it is linked -Bsymbolic, the global group, then
   the HLE. A missing function becomes an export that reports it; a missing object, 0 */
static uint64_t resolve(GuestLibrary* l, uint32_t index, bool function, char* error, size_t error_size) {
    const Elf64_Sym* s = &l->symbols[index];
    const char* name = l->strings + s->st_name;
    if (s->st_shndx != SHN_UNDEF && (l->symbolic || ELF64_ST_BIND(s->st_info) == STB_LOCAL))
        return symbol_address(l, s);
    uint64_t a = linker_symbol(NULL, name);
    if (!a && s->st_shndx != SHN_UNDEF) a = symbol_address(l, s);
    if (!a && ELF64_ST_BIND(s->st_info) != STB_WEAK) {
        if (function || ELF64_ST_TYPE(s->st_info) == STT_FUNC || ELF64_ST_TYPE(s->st_info) == STT_NOTYPE)
            return hle_missing(name);
        snprintf(error, error_size, "cannot locate symbol \"%s\" referenced by \"%s\"", name, l->name);
    }
    return a;
}

/* ---- relocations ---- */

/* ---- thread-local storage ----
   Each library's PT_TLS is a module of dynamic TLS, as bionic gives a dlopen'd library's: each thread gets the
   module's block (its .tdata image, then zeros for .tbss) the first time it reaches it, through a TLS descriptor's
   resolver or __tls_get_addr (hle_dl.c). Static TLS (TPREL relocations of an executable's initial-exec model) is
   not here */
enum { TLS_MODULES = 256 };
typedef struct {
    uint64_t image, filesz, memsz, align;
} TlsModule;
static TlsModule tls_modules[TLS_MODULES]; /* written before a module's number is given out, then read only */
static unsigned tls_count;
typedef struct {
    unsigned char* blocks[TLS_MODULES];
    void* raw[TLS_MODULES];
} TlsThread;
static pthread_key_t tls_key;
static pthread_once_t tls_once = PTHREAD_ONCE_INIT;
static void tls_thread_free(void* p) {
    TlsThread* t = p;
    for (unsigned i = 0; i < TLS_MODULES; ++i) free(t->raw[i]);
    free(t);
}
static void tls_init(void) { pthread_key_create(&tls_key, tls_thread_free); }

uint64_t linker_tls_address(unsigned module, uint64_t offset) {
    if (!module || module > tls_count) return 0;
    pthread_once(&tls_once, tls_init);
    TlsThread* t = pthread_getspecific(tls_key);
    if (!t) {
        t = calloc(1, sizeof *t);
        if (!t) return 0;
        pthread_setspecific(tls_key, t);
    }
    if (!t->blocks[module - 1]) {
        const TlsModule* m = &tls_modules[module - 1];
        uint64_t align = m->align > 16 ? m->align : 16;
        unsigned char* raw = malloc((size_t)(m->memsz + align));
        if (!raw) return 0;
        unsigned char* block = (unsigned char*)(((uintptr_t)raw + (align - 1)) & ~(uintptr_t)(align - 1));
        memcpy(block, (const void*)(uintptr_t)m->image, (size_t)m->filesz);
        memset(block + m->filesz, 0, (size_t)(m->memsz - m->filesz));
        t->raw[module - 1] = raw;
        t->blocks[module - 1] = block;
    }
    return (uint64_t)(uintptr_t)t->blocks[module - 1] + offset;
}

/* a TLS relocation's symbol: its module and its offset in the module's block (the library's own block for none) */
static bool tls_symbol(const GuestLibrary* l, uint32_t index, unsigned* module, uint64_t* value) {
    if (!index) {
        *module = l->tls_module;
        *value = 0;
        return l->tls_module != 0;
    }
    const Elf64_Sym* s = &l->symbols[index];
    if (s->st_shndx != SHN_UNDEF) {
        *module = l->tls_module;
        *value = s->st_value;
        return l->tls_module != 0;
    }
    const char* name = l->strings + s->st_name;
    for (const GuestLibrary* o = g.first; o; o = o->next) {
        const Elf64_Sym* d = o->tls_module ? defined(o, name) : NULL;
        if (d && ELF64_ST_TYPE(d->st_info) == STT_TLS) {
            *module = o->tls_module;
            *value = d->st_value;
            return true;
        }
    }
    return false;
}

typedef struct {
    GuestLibrary* l;
    char* error;
    size_t error_size;
    bool failed;
} Relocating;

static void store64(uint64_t at, uint64_t value) {
    memcpy((void*)(uintptr_t)at, &value, 8);
}

static void apply(Relocating* r, uint64_t offset, uint64_t info, int64_t addend) {
    GuestLibrary* l = r->l;
    uint64_t at = l->base + offset;
    uint32_t type = (uint32_t)ELF64_R_TYPE(info), sym = (uint32_t)ELF64_R_SYM(info);
    switch (type) {
    case R_AARCH64_NONE: break;
    case R_AARCH64_RELATIVE: store64(at, l->base + (uint64_t)addend); break;
    case R_AARCH64_GLOB_DAT:
    case R_AARCH64_JUMP_SLOT:
    case R_AARCH64_ABS64: {
        uint64_t s = resolve(l, sym, type == R_AARCH64_JUMP_SLOT, r->error, r->error_size);
        if (!s && r->error[0]) r->failed = true;
        store64(at, s ? s + (uint64_t)addend : 0);
        break;
    }
    case R_AARCH64_IRELATIVE: {
        /* the resolver, a guest function, picks the implementation (by the CPU's features, which it reads itself) */
        uint64_t args[1] = {0};
        store64(at, bridge_call(l->base + (uint64_t)addend, args, 1));
        break;
    }
    case R_AARCH64_TLSDESC:
    case R_AARCH64_TLS_DTPMOD64:
    case R_AARCH64_TLS_DTPREL64: {
        unsigned module;
        uint64_t value;
        if (!tls_symbol(l, sym, &module, &value)) {
            snprintf(r->error, r->error_size, "\"%s\": a TLS relocation's symbol %u has no TLS module", l->name, (unsigned)sym);
            r->failed = true;
            break;
        }
        value += (uint64_t)addend;
        if (type == R_AARCH64_TLS_DTPMOD64) {
            store64(at, module);
        } else if (type == R_AARCH64_TLS_DTPREL64) {
            store64(at, value);
        } else {
            /* the descriptor: the resolver, and its argument (the module above the offset's 48 bits) */
            store64(at, hle_symbol("__snx_tlsdesc"));
            store64(at + 8, (uint64_t)module << 48 | (value & 0xffffffffffffull));
        }
        break;
    }
    default:
        snprintf(r->error, r->error_size, "\"%s\": unknown relocation type %u", l->name, (unsigned)type);
        r->failed = true;
        break;
    }
}

static void apply_rela(Relocating* r, const Elf64_Rela* rela, size_t count) {
    for (size_t i = 0; i < count && !r->failed; ++i) apply(r, rela[i].r_offset, rela[i].r_info, rela[i].r_addend);
}

static int64_t sleb(const unsigned char** p, const unsigned char* end) {
    int64_t v = 0;
    unsigned shift = 0;
    unsigned char byte = 0x80;
    while (*p < end && (byte & 0x80)) {
        byte = *(*p)++;
        if (shift < 64) v |= (int64_t)((uint64_t)(byte & 0x7f) << shift);
        shift += 7;
    }
    if (shift < 64 && (byte & 0x40)) v |= (int64_t)(~(uint64_t)0 << shift);
    return v;
}

/* Android's packed relocations: "APS2", then SLEB128 numbers: the count, the first offset, then groups. A group has
   its size and flags, then what the flags say the group shares (the offset delta, the info, the addend delta), then
   each relocation's own */
static void apply_packed(Relocating* r, const unsigned char* p, size_t size) {
    enum { BY_INFO = 1, BY_OFFSET_DELTA = 2, BY_ADDEND = 4, HAS_ADDEND = 8 };
    const unsigned char* end = p + size;
    if (size < 4 || memcmp(p, "APS2", 4)) {
        snprintf(r->error, r->error_size, "\"%s\": bad packed relocations", r->l->name);
        r->failed = true;
        return;
    }
    p += 4;
    int64_t count = sleb(&p, end);
    uint64_t offset = (uint64_t)sleb(&p, end), info = 0;
    int64_t addend = 0;
    while (count > 0 && p < end && !r->failed) {
        int64_t group = sleb(&p, end), flags = sleb(&p, end);
        int64_t offset_delta = flags & BY_OFFSET_DELTA ? sleb(&p, end) : 0;
        if (flags & BY_INFO) info = (uint64_t)sleb(&p, end);
        if ((flags & HAS_ADDEND) && (flags & BY_ADDEND)) addend += sleb(&p, end);
        else if (!(flags & HAS_ADDEND)) addend = 0;
        for (int64_t i = 0; i < group && count > 0; ++i, --count) {
            offset += flags & BY_OFFSET_DELTA ? (uint64_t)offset_delta : (uint64_t)sleb(&p, end);
            if (!(flags & BY_INFO)) info = (uint64_t)sleb(&p, end);
            if ((flags & HAS_ADDEND) && !(flags & BY_ADDEND)) addend += sleb(&p, end);
            apply(r, offset, info, addend);
        }
    }
}

/* DT_RELR: relative relocations as an address, then bitmaps of the words after it */
static void apply_relr(GuestLibrary* l, const uint64_t* relr, size_t count) {
    uint64_t where = 0;
    for (size_t i = 0; i < count; ++i) {
        uint64_t e = relr[i];
        if (!(e & 1)) {
            where = l->base + e;
            uint64_t v;
            memcpy(&v, (void*)(uintptr_t)where, 8);
            store64(where, v + l->base);
            where += 8;
            continue;
        }
        for (uint64_t bits = e >> 1, at = where; bits; bits >>= 1, at += 8)
            if (bits & 1) {
                uint64_t v;
                memcpy(&v, (void*)(uintptr_t)at, 8);
                store64(at, v + l->base);
            }
        where += 63 * 8;
    }
}

/* ---- loading ---- */

static GuestLibrary* find_loaded(const char* path) {
    const char* name = base_name(path);
    for (GuestLibrary* l = g.first; l; l = l->next)
        if (!strcmp(l->path, path) || !strcmp(l->name, name) || !strcmp(base_name(l->path), name)) return l;
    return NULL;
}

/* a segment's access: its code is read, never run, by the host */
static int access_of(uint32_t flags) {
    return (flags & PF_R ? HOST_READ : 0) | (flags & PF_W ? HOST_READ | HOST_WRITE : 0) | (flags & PF_X ? HOST_READ : 0);
}

static GuestLibrary* load_file(const char* path, char* error, size_t error_size);

/* a DT_NEEDED name: an HLE library (NULL, no error), the app's own, or an error */
static GuestLibrary* load_needed(const char* name, bool* hle, char* error, size_t error_size) {
    *hle = false;
    GuestLibrary* l = find_loaded(name);
    if (l) return l;
    char* path = g.library_dir && !strchr(name, '/') ? snx_format("%s/%s", g.library_dir, name) : copy(name);
    if (path && snx_is_file(path)) {
        l = load_file(path, error, error_size);
        free(path);
        return l;
    }
    free(path);
    if (hle_is_system_library(base_name(name))) {
        *hle = true;
        return NULL;
    }
    snprintf(error, error_size, "library \"%s\" not found", name);
    return NULL;
}

static GuestLibrary* load_file(const char* path, char* error, size_t error_size) {
    int fd = open(path, LIBRARY_OPEN_FLAGS);
    if (fd < 0) {
        snprintf(error, error_size, "library \"%s\" not found", path);
        return NULL;
    }
    Elf64_Ehdr eh;
    Elf64_Phdr ph[64];
    if (plat_pread_fd(fd, &eh, sizeof eh, 0) != (int64_t)sizeof eh || memcmp(eh.e_ident, ELFMAG, SELFMAG) ||
        eh.e_ident[EI_CLASS] != ELFCLASS64 || eh.e_ident[EI_DATA] != ELFDATA2LSB || eh.e_machine != EM_AARCH64 ||
        eh.e_type != ET_DYN || eh.e_phnum > 64 || eh.e_phentsize != sizeof(Elf64_Phdr) ||
        plat_pread_fd(fd, ph, eh.e_phnum * sizeof *ph, eh.e_phoff) != (int64_t)(eh.e_phnum * sizeof *ph)) {
        snprintf(error, error_size, "\"%s\" is not a 64-bit ARM shared library", path);
        close(fd);
        return NULL;
    }
    uint64_t lo = UINT64_MAX, hi = 0, align = 0x10000;
    const Elf64_Phdr* dyn = NULL;
    for (unsigned i = 0; i < eh.e_phnum; ++i) {
        if (ph[i].p_type == PT_DYNAMIC) dyn = &ph[i];
        if (ph[i].p_type != PT_LOAD) continue;
        if (ph[i].p_vaddr < lo) lo = ph[i].p_vaddr;
        if (ph[i].p_vaddr + ph[i].p_memsz > hi) hi = ph[i].p_vaddr + ph[i].p_memsz;
        if (ph[i].p_align > align) align = ph[i].p_align;
    }
    lo &= ~(uint64_t)0xfff;
    hi = (hi + 0xffff) & ~(uint64_t)0xffff;
    uint64_t span = hi - lo, at = bridge_window_reserve(span, align);
    if (!dyn || !at || plat_mem_commit((void*)(uintptr_t)at, span)) {
        snprintf(error, error_size, "\"%s\": %s", path, dyn ? "no room in the code window" : "no PT_DYNAMIC");
        close(fd);
        return NULL;
    }
    GuestLibrary* l = snx_alloc(sizeof *l);
    memset(l, 0, sizeof *l);
    l->base = at - lo;
    l->size = span;
    /* the program headers in its image: PT_PHDR's, else where the first segment, read from offset 0, holds them */
    l->phnum = eh.e_phnum;
    l->phdr = l->base + eh.e_phoff;
    for (unsigned i = 0; i < eh.e_phnum; ++i) {
        if (ph[i].p_type == PT_PHDR) l->phdr = l->base + ph[i].p_vaddr;
        else if (ph[i].p_type == PT_LOAD && ph[i].p_offset == 0 && i == 0)
            l->phdr = l->base + ph[i].p_vaddr + eh.e_phoff;
    }
    l->path = copy(path);
    for (unsigned i = 0; i < eh.e_phnum; ++i) {
        if (ph[i].p_type != PT_LOAD || !ph[i].p_filesz) continue;
        if (plat_pread_fd(fd, (void*)(uintptr_t)(l->base + ph[i].p_vaddr), ph[i].p_filesz, ph[i].p_offset) !=
            (int64_t)ph[i].p_filesz) {
            snprintf(error, error_size, "\"%s\": cannot read its segments", path);
            close(fd);
            return NULL;
        }
    }
    close(fd);
    /* its TLS segment, a module */
    for (unsigned i = 0; i < eh.e_phnum; ++i)
        if (ph[i].p_type == PT_TLS && ph[i].p_memsz && tls_count < TLS_MODULES) {
            tls_modules[tls_count] = (TlsModule){l->base + ph[i].p_vaddr, ph[i].p_filesz, ph[i].p_memsz, ph[i].p_align};
            l->tls_module = ++tls_count;
        }

    /* the dynamic section */
    l->dynamic = (const Elf64_Dyn*)(uintptr_t)(l->base + dyn->p_vaddr);
    const Elf64_Rela *rela = NULL, *jmprel = NULL;
    const unsigned char* packed = NULL;
    const uint64_t* relr = NULL;
    size_t rela_size = 0, jmprel_size = 0, packed_size = 0, relr_size = 0, init_size = 0, preinit_size = 0;
    uint64_t init = 0, init_array = 0, preinit_array = 0, soname = 0;
    for (const Elf64_Dyn* d = l->dynamic; d->d_tag != DT_NULL; ++d) {
        uint64_t v = d->d_un.d_val, p = l->base + v;
        switch (d->d_tag) {
        case DT_STRTAB: l->strings = (const char*)(uintptr_t)p; break;
        case DT_SYMTAB: l->symbols = (const Elf64_Sym*)(uintptr_t)p; break;
        case DT_HASH: l->hash = (const uint32_t*)(uintptr_t)p; break;
        case DT_GNU_HASH: l->gnu_hash = (const uint32_t*)(uintptr_t)p; break;
        case DT_RELA: rela = (const Elf64_Rela*)(uintptr_t)p; break;
        case DT_RELASZ: rela_size = v; break;
        case DT_JMPREL: jmprel = (const Elf64_Rela*)(uintptr_t)p; break;
        case DT_PLTRELSZ: jmprel_size = v; break;
        case DT_ANDROID_RELA: packed = (const unsigned char*)(uintptr_t)p; break;
        case DT_ANDROID_RELASZ: packed_size = v; break;
        case DT_RELR: relr = (const uint64_t*)(uintptr_t)p; break;
        case DT_RELRSZ: relr_size = v; break;
        case DT_INIT: init = p; break;
        case DT_INIT_ARRAY: init_array = p; break;
        case DT_INIT_ARRAYSZ: init_size = v; break;
        case DT_PREINIT_ARRAY: preinit_array = p; break;
        case DT_PREINIT_ARRAYSZ: preinit_size = v; break;
        case DT_SONAME: soname = v; break;
        case DT_SYMBOLIC: l->symbolic = true; break;
        case DT_FLAGS: if (v & DF_SYMBOLIC) l->symbolic = true; break;
        default: break;
        }
    }
    l->name = copy(soname && l->strings ? l->strings + soname : base_name(path));
    if (!l->symbols || !l->strings || (!l->hash && !l->gnu_hash)) {
        snprintf(error, error_size, "\"%s\": no symbol table", path);
        return NULL;
    }

    /* the libraries it needs, loaded first */
    for (const Elf64_Dyn* d = l->dynamic; d->d_tag != DT_NULL; ++d) {
        if (d->d_tag != DT_NEEDED) continue;
        bool hle;
        GuestLibrary* n = load_needed(l->strings + d->d_un.d_val, &hle, error, error_size);
        if (!n && !hle) return NULL;
        l->needed = snx_realloc(l->needed, (l->needed_count + 1) * sizeof *l->needed);
        l->needed[l->needed_count++] = n;
    }
    /* in the global group from here on, so that its own relocations find it */
    GuestLibrary** tail = &g.first;
    while (*tail) tail = &(*tail)->next;
    *tail = l;

    error[0] = 0;
    Relocating r = {l, error, error_size, false};
    if (packed) apply_packed(&r, packed, packed_size);
    if (relr) apply_relr(l, relr, relr_size / 8);
    if (rela) apply_rela(&r, rela, rela_size / sizeof *rela);
    if (jmprel) apply_rela(&r, jmprel, jmprel_size / sizeof *jmprel);
    if (r.failed) {
        /* out of the global group again: a library that did not load has no symbols to give (its code is not
           the JIT's, and its constructors did not run) */
        for (GuestLibrary** at = &g.first; *at; at = &(*at)->next)
            if (*at == l) {
                *at = l->next;
                break;
            }
        return NULL;
    }

    /* the segments' protections, and their code for the JIT */
    for (unsigned i = 0; i < eh.e_phnum; ++i) {
        if (ph[i].p_type != PT_LOAD) continue;
        uint64_t s = (l->base + ph[i].p_vaddr) & ~(uint64_t)0xfff;
        uint64_t e = (l->base + ph[i].p_vaddr + ph[i].p_memsz + 0xfff) & ~(uint64_t)0xfff;
        plat_mem_protect((void*)(uintptr_t)s, e - s, access_of(ph[i].p_flags));
        if (ph[i].p_flags & PF_X) bridge_add_code(l->base + ph[i].p_vaddr, (ph[i].p_memsz + 3) & ~(uint64_t)3);
    }
    for (unsigned i = 0; i < eh.e_phnum; ++i)
        if (ph[i].p_type == PT_GNU_RELRO) {
            uint64_t s = (l->base + ph[i].p_vaddr) & ~(uint64_t)0xfff;
            uint64_t e = (l->base + ph[i].p_vaddr + ph[i].p_memsz) & ~(uint64_t)0xfff;
            if (e > s) plat_mem_protect((void*)(uintptr_t)s, e - s, HOST_READ);
        }

    /* its constructors, as bionic runs them: DT_PREINIT_ARRAY (an executable's), DT_INIT, then DT_INIT_ARRAY. Each
       gets argc, argv and envp, which a library's seldom reads */
    uint64_t args[3] = {0, 0, 0};
    for (size_t i = 0; i < preinit_size / 8; ++i) {
        uint64_t f;
        memcpy(&f, (const void*)(uintptr_t)(preinit_array + 8 * i), 8);
        if (f && f != UINT64_MAX) bridge_call(f, args, 3);
    }
    if (init) bridge_call(init, args, 3);
    for (size_t i = 0; i < init_size / 8; ++i) {
        uint64_t f;
        memcpy(&f, (const void*)(uintptr_t)(init_array + 8 * i), 8);
        if (f && f != UINT64_MAX) bridge_call(f, args, 3);
    }
    if (linker_verbose)
        fprintf(stderr, "scarletnx-android: loaded %s at 0x%llx\n", l->name, (unsigned long long)l->base);
    return l;
}

GuestLibrary* linker_load(const char* path, char* error, size_t error_size) {
    pthread_mutex_lock(&g.lock);
    GuestLibrary* l = find_loaded(path);
    if (!l) {
        bool hle;
        l = load_needed(path, &hle, error, error_size);
        if (hle) snprintf(error, error_size, "\"%s\" is a system library", path);
    }
    pthread_mutex_unlock(&g.lock);
    return l;
}

int linker_each(int (*fn)(void* context, uint64_t base, const char* name, uint64_t phdr, unsigned count),
                void* context) {
    for (GuestLibrary* l = g.first; l; l = l->next) {
        int r = fn(context, l->base, l->path, l->phdr, l->phnum);
        if (r) return r;
    }
    return 0;
}

GuestLibrary* linker_owner(uint64_t address) {
    for (GuestLibrary* l = g.first; l; l = l->next)
        if (address >= l->base && address < l->base + l->size + 0x10000) return l;
    return NULL;
}

uint64_t linker_next_symbol(uint64_t caller, const char* name) {
    GuestLibrary* owner = linker_owner(caller);
    for (GuestLibrary* l = owner ? owner->next : g.first; l; l = l->next) {
        const Elf64_Sym* s = defined(l, name);
        if (s) return symbol_address(l, s);
    }
    return hle_symbol(name);
}

bool linker_address(uint64_t address, const char** library, uint64_t* base, const char** symbol,
                    uint64_t* symbol_at) {
    for (GuestLibrary* l = g.first; l; l = l->next) {
        if (address < l->base || address >= l->base + l->size + 0x10000) continue;
        *library = l->name;
        *base = l->base;
        *symbol = NULL;
        *symbol_at = 0;
        uint32_t count = l->hash ? l->hash[1] : 0;
        for (uint32_t i = 1; i < count; ++i) {
            const Elf64_Sym* s = &l->symbols[i];
            uint64_t a = l->base + s->st_value;
            uint64_t end = a + (s->st_size ? s->st_size : 1);
            if (s->st_shndx != SHN_UNDEF && a <= address && a > *symbol_at && address < end) {
                *symbol = l->strings + s->st_name;
                *symbol_at = a;
            }
        }
        return true;
    }
    return false;
}
