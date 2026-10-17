/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The JIT's loader (code_loader.h): a code arena, and the LLVM backend's ELF objects linked into it (COFF objects:
   code_loader_coff.c) */
#ifdef __linux__
#define _GNU_SOURCE /* memfd_create, syscall */
#else
#define _POSIX_C_SOURCE 200809L /* snprintf */
#endif
#include "common/cpu/recompiler/llvm/code_loader_int.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include "common/cpu/recompiler/llvm/elf_object.h"
#endif
#if defined(__linux__) && defined(__aarch64__)
#include <sys/syscall.h>
/* <linux/membarrier.h>'s commands (Linux 4.16), by number for C libraries whose headers predate them */
#define MEMBARRIER_SYNC_CORE          (1 << 5) /* MEMBARRIER_CMD_PRIVATE_EXPEDITED_SYNC_CORE */
#define MEMBARRIER_REGISTER_SYNC_CORE (1 << 6) /* MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_SYNC_CORE */
#endif

/* the table keeps addresses in SnxLinkNames' size_t values */
typedef char snx_code_loader_needs_64_bits[sizeof(size_t) >= 8 ? 1 : -1];

/* the machine whose code this process runs, as an ELF e_machine: x86-64's 62, AArch64's 183; Windows' x86-64 COFF;
   0 where the loader cannot run what it loads */
#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
#define HOST_MACHINE SNX_CODE_COFF_X64
#elif defined(_WIN32)
#define HOST_MACHINE 0u
#elif defined(__x86_64__) || defined(_M_X64)
#define HOST_MACHINE 62u
#elif defined(__aarch64__) || defined(_M_ARM64)
#define HOST_MACHINE 183u
#else
#define HOST_MACHINE 0u
#endif

/* the regions (code_loader_int.h), by their short names here */
#define CODE    SNX_CODE_TEXT
#define RODATA  SNX_CODE_RODATA
#define DATA    SNX_CODE_DATA
#define REGIONS SNX_CODE_REGIONS
typedef SnxCodeRegion Region;

#ifdef _WIN32
/* Windows 10 1803's placeholder calls, found at run time (memoryapi.h declares them only for newer targets) */
typedef void*(WINAPI* VirtualAlloc2Fn)(HANDLE, void*, SIZE_T, ULONG, ULONG, void*, ULONG);
typedef void*(WINAPI* MapViewOfFile3Fn)(HANDLE, HANDLE, void*, ULONG64, SIZE_T, ULONG, ULONG, void*, ULONG);
#ifndef MEM_RESERVE_PLACEHOLDER
#define MEM_RESERVE_PLACEHOLDER  0x00040000
#define MEM_REPLACE_PLACEHOLDER  0x00004000
#define MEM_PRESERVE_PLACEHOLDER 0x00000002
#endif

/* The arena: a placeholder reservation cut into the three regions. Code and read-only data are views of a section of
   their own each, into their placeholders (read-execute, read-only), and again read-write anywhere for the loader;
   data is reserved, committed as loads reach it. 0, or -1 with the reason in error */
static int windows_arena(SnxCodeLoader* loader, char* error, size_t error_size) {
    HMODULE kernelbase = GetModuleHandleW(L"kernelbase.dll");
    VirtualAlloc2Fn alloc2 = kernelbase ? (VirtualAlloc2Fn)(void*)GetProcAddress(kernelbase, "VirtualAlloc2") : NULL;
    MapViewOfFile3Fn map3 = kernelbase ? (MapViewOfFile3Fn)(void*)GetProcAddress(kernelbase, "MapViewOfFile3") : NULL;
    static const ULONG protection[REGIONS] = {PAGE_EXECUTE_READ, PAGE_READONLY, 0};
    unsigned char* base;
    int i;
    if (!alloc2 || !map3) {
        snprintf(error, error_size,
                 "the JIT's code arena needs Windows 10 1803 or later (VirtualAlloc2, MapViewOfFile3)");
        return -1;
    }
    base = (unsigned char*)alloc2(NULL, NULL, loader->reserved, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS,
                                  NULL, 0);
    if (!base) {
        snprintf(error, error_size, "cannot reserve %lu MiB of address space for code",
                 (unsigned long)(loader->reserved >> 20));
        return -1;
    }
    loader->base = base;
    /* three placeholders: splitting one off the front leaves the rest a placeholder too */
    if (!VirtualFree(base, loader->region[CODE].size, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) ||
        !VirtualFree(base + loader->region[RODATA].start, loader->region[RODATA].size,
                     MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
        snprintf(error, error_size, "cannot divide the code arena's reservation (%lu)", (unsigned long)GetLastError());
        return -1;
    }
    for (i = CODE; i <= RODATA; ++i) {
        size_t size = loader->region[i].size;
        HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_EXECUTE_READWRITE,
                                            (DWORD)((uint64_t)size >> 32), (DWORD)size, NULL);
        void *view = NULL, *write = NULL;
        if (section) {
            view = map3(section, NULL, base + loader->region[i].start, 0, size, MEM_REPLACE_PLACEHOLDER, protection[i],
                        NULL, 0);
            if (view) write = map3(section, NULL, NULL, 0, size, 0, PAGE_READWRITE, NULL, 0);
            CloseHandle(section); /* the views keep it */
        }
        if (!view || !write) {
            if (view) UnmapViewOfFile(view);
            snprintf(error, error_size, "cannot map %lu MiB of code memory twice (%lu)", (unsigned long)(size >> 20),
                     (unsigned long)GetLastError());
            return -1;
        }
        loader->region[i].write = (unsigned char*)write;
    }
    if (!alloc2(NULL, base + loader->region[DATA].start, loader->region[DATA].size,
                MEM_RESERVE | MEM_REPLACE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0)) {
        snprintf(error, error_size, "cannot reserve the code arena's data (%lu)", (unsigned long)GetLastError());
        return -1;
    }
    loader->region[DATA].write = base + loader->region[DATA].start;
    return 0;
}
#else
/* Shared memory of size bytes, mapped at at (in the arena's reservation) readable, and executable when asked, and once
   more read-write where the system puts it: that view, or NULL with the reason in error */
static unsigned char* map_views(unsigned char* at, size_t size, int executable, char* error, size_t error_size) {
    void *view, *write = MAP_FAILED;
    int fd;
#if defined(__linux__)
    fd = memfd_create("snx-jit-code", MFD_CLOEXEC);
#elif defined(SHM_ANON)
    fd = shm_open(SHM_ANON, O_RDWR | O_CLOEXEC, 0600);
#else
    fd = -1;
#endif
    if (fd < 0) {
        snprintf(error, error_size, "no anonymous shared memory for the code arena");
        return NULL;
    }
    if (!ftruncate(fd, (off_t)size)) {
        view = mmap(at, size, PROT_READ | (executable ? PROT_EXEC : 0), MAP_SHARED | MAP_FIXED, fd, 0);
        if (view == (void*)at) write = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    }
    close(fd); /* the mappings keep the memory */
    if (write == MAP_FAILED) {
        snprintf(error, error_size, "cannot map %lu MiB of code memory twice", (unsigned long)(size >> 20));
        return NULL;
    }
    return (unsigned char*)write;
}
#endif

/* Whether code written by this thread can be published to threads running on other cores (code_loader.h). An
   AArch64 core may hold instructions it fetched before the code was written, which neither the writer's cache
   maintenance nor the reader's acquire load discards: each core needs a context synchronization event, which
   membarrier's SYNC_CORE command makes every thread of the process take (an ISB on the way back to user space), and
   which the process registers for once. Without it a load would hand out code another core might run stale, so the
   loader refuses and the JIT stays off (the interpreter runs those functions) */
static int can_publish(unsigned machine, char* error, size_t error_size) {
    if (machine != HOST_MACHINE) return 1; /* nothing loaded runs: tests of another machine's objects */
#if defined(__aarch64__)
#if defined(__linux__) && defined(__NR_membarrier)
    if (!syscall(__NR_membarrier, MEMBARRIER_REGISTER_SYNC_CORE, 0u, 0)) return 1;
    snprintf(error, error_size, "the kernel has no membarrier SYNC_CORE (Linux 4.16), which publishing JIT code needs");
#else
    snprintf(error, error_size, "no way to synchronize other cores with new JIT code on this system");
#endif
    return 0;
#else
    (void)error;
    (void)error_size;
    return 1; /* x86-64 fetches what stores wrote, on every core (code_loader.h) */
#endif
}

SnxCodeLoader* snx_code_loader_new(size_t reserve, char* error, size_t error_size) {
    if (!HOST_MACHINE) {
        snprintf(error, error_size, "the JIT loader runs on x86-64 and AArch64 hosts");
        return NULL;
    }
    return snx_code_loader_new_machine(reserve, HOST_MACHINE, error, error_size);
}

SnxCodeLoader* snx_code_loader_new_machine(size_t reserve, unsigned machine, char* error, size_t error_size) {
    SnxCodeLoader* loader;
    size_t page = snx_vm_page_size();
    int i;
    if (!reserve) reserve = SNX_CODE_ARENA_DEFAULT;
    if (reserve <= (size_t)1 << 31) reserve = (reserve + 8 * page - 1) / (8 * page) * (8 * page); /* whole pages */
    if (reserve > (size_t)1 << 31) {
        snprintf(error, error_size, "a code arena holds at most 2 GiB, so 32-bit displacements reach across it");
        return NULL;
    }
    if (!can_publish(machine, error, error_size)) return NULL;
    loader = (SnxCodeLoader*)calloc(1, sizeof(*loader));
    if (!loader) {
        snprintf(error, error_size, "out of memory");
        return NULL;
    }
    loader->reserved = reserve;
    loader->page = page;
    loader->machine = machine;
    /* code is most of what a unit holds. Read-only data is its jump tables, constants, function table and GOT, and
       generated code writes no data of its own (nor does the runtime library) */
    loader->region[CODE].size = reserve / 8 * 5;
    loader->region[RODATA].start = loader->region[CODE].size;
    loader->region[RODATA].size = reserve / 8 * 2;
    loader->region[DATA].start = loader->region[RODATA].start + loader->region[RODATA].size;
    loader->region[DATA].size = reserve - loader->region[DATA].start;
#ifdef _WIN32
    (void)i;
    if (windows_arena(loader, error, error_size)) {
        snx_code_loader_free(loader);
        return NULL;
    }
#else
    loader->base = (unsigned char*)snx_vm_reserve(reserve);
    if (!loader->base) {
        snprintf(error, error_size, "cannot reserve %lu MiB of address space for code", (unsigned long)(reserve >> 20));
        free(loader);
        return NULL;
    }
    for (i = CODE; i <= RODATA; ++i) {
        loader->region[i].write =
            map_views(loader->base + loader->region[i].start, loader->region[i].size, i == CODE, error, error_size);
        if (!loader->region[i].write) {
            snx_code_loader_free(loader);
            return NULL;
        }
    }
    loader->region[DATA].write = loader->base + loader->region[DATA].start;
#endif
    return loader;
}

void snx_code_loader_free(SnxCodeLoader* loader) {
    if (!loader) return;
#ifdef _WIN32
    /* each part of the arena goes as it was made: the views, the data reservation, a placeholder left over */
    {
        int i;
        for (i = CODE; i <= RODATA; ++i)
            if (loader->region[i].write) {
                UnmapViewOfFile(loader->region[i].write);
                UnmapViewOfFile(loader->base + loader->region[i].start);
            }
        if (loader->base)
            for (i = CODE; i < REGIONS; ++i) VirtualFree(loader->base + loader->region[i].start, 0, MEM_RELEASE);
    }
#else
    if (loader->region[CODE].write) munmap(loader->region[CODE].write, loader->region[CODE].size);
    if (loader->region[RODATA].write) munmap(loader->region[RODATA].write, loader->region[RODATA].size);
    snx_vm_release(loader->base, loader->reserved); /* the arena's views with it */
#endif
    snx_link_names_free(&loader->symbols);
    free(loader);
}

int snx_code_define(SnxCodeLoader* loader, const char* name, SnxU64 address) {
    size_t found;
    if (snx_link_find(&loader->symbols, name, &found)) return -1;
    snx_link_insert(&loader->symbols, name, (size_t)address);
    return 0;
}

void snx_code_set_resolver(SnxCodeLoader* loader, SnxU64 (*resolver)(void* user, const char* name), void* user) {
    loader->resolver = resolver;
    loader->resolver_user = user;
}

int snx_code_find(const SnxCodeLoader* loader, const char* name, SnxU64* address) {
    if (snx_code_lookup(loader, name, address)) return 1;
    if (!loader->resolver) return 0;
    *address = loader->resolver(loader->resolver_user, name);
    return *address != 0;
}

int snx_code_lookup(const SnxCodeLoader* loader, const char* name, SnxU64* address) {
    size_t found;
    if (!snx_link_find(&loader->symbols, name, &found)) return 0;
    *address = found;
    return 1;
}

size_t snx_code_used(const SnxCodeLoader* loader) {
    return loader->region[CODE].used + loader->region[RODATA].used + loader->region[DATA].used;
}

unsigned char* snx_code_at(const SnxCodeLoader* loader, uint64_t address) {
    size_t offset = (size_t)(address - (uint64_t)(size_t)loader->base);
    const Region* r = &loader->region[offset >= loader->region[DATA].start     ? DATA
                                      : offset >= loader->region[RODATA].start ? RODATA
                                                                               : CODE];
    return r->write + (offset - r->start);
}

uint64_t snx_code_next_free(const SnxCodeLoader* loader, int region) {
    return (uint64_t)(size_t)loader->base + loader->region[region].start + loader->region[region].used;
}

/* data pages become read-write as loads reach them; pages that are stay so, used or not */
int snx_code_commit(SnxCodeLoader* loader, uint64_t end, char* error, size_t error_size) {
    uint64_t start = (uint64_t)(size_t)loader->base + loader->region[DATA].start;
    uint64_t need = (end - start + loader->page - 1) / loader->page * loader->page;
    if (need <= loader->committed) return 0;
    if (snx_vm_protect(loader->region[DATA].write + loader->committed, (size_t)(need - loader->committed),
                       SNX_VM_READ_WRITE)) {
        snprintf(error, error_size, "cannot commit %lu KiB of data memory",
                 (unsigned long)((need - loader->committed) >> 10));
        return -1;
    }
    loader->committed = (size_t)need;
    return 0;
}

/* The code is complete: visible to instruction fetch before any address goes out (code_loader.h). On AArch64 the
   clear-cache builtin cleans the data cache to the point of unification and invalidates the instruction cache by
   virtual address, and it is given the executable view's addresses, not the read-write view's the bytes were written
   through. Data caches behave as physically indexed and tagged, so the clean finds the written lines through either
   view (it needs a readable one: the arena's code is read-execute); an instruction cache may be virtually indexed, so
   only the addresses instruction fetch uses invalidate the lines it would hit. Both operations reach every core of the
   inner shareable domain. The read-only data and GOT need none: no instruction is fetched from them, and data accesses
   are coherent between the views */
void snx_code_publish(const SnxCodeLoader* loader, uint64_t start, uint64_t end) {
    if (end <= start) return;
    snx_vm_flush_code((void*)(size_t)start, (size_t)(end - start));
#if defined(__linux__) && defined(__aarch64__) && defined(__NR_membarrier)
    /* and every other core of the process drops what it fetched before (can_publish) */
    if (loader->machine == HOST_MACHINE) syscall(__NR_membarrier, MEMBARRIER_SYNC_CORE, 0u, 0);
#else
    (void)loader;
#endif
}

#ifndef _WIN32
/* A stub is how a call reaches a table symbol beyond its displacement, through the stub area after the load's code.
   x86-64: jmp *slot(%rip) through the symbol's GOT slot, and two int3 to the next stub (8 bytes), for a symbol
   outside the arena. AArch64: an elf_object.c veneer (12 bytes), for any table symbol, since B and BL reach 128 MiB
   and the arena is larger: direct (ADRP, ADD) to one in the arena, indirect through its GOT slot (ADRP, LDR) to one
   outside, which ADRP's 4 GiB may not reach */
#define STUB_SIZE_X86 8
/* where each load's code starts: a line no running code shares, so writing it never touches a line another core may
   be executing from */
#define CODE_LINE     64

/* One snx_code_load call. Addresses are absolute, the arena's [arena_start, arena_end) */
typedef struct {
    SnxElfObject* objs;
    size_t count;
    unsigned machine;
    SnxLinkNames globals; /* the unit's own: name -> object << 32 | symbol index */
    SnxLinkNames got;     /* "object:symbol" of a unit definition, or "@address" of a table symbol -> GOT slot */
    SnxLinkNames stubs;   /* "@address" of a table symbol that a call may not reach -> stub */
    size_t got_count, stub_count;
    uint64_t arena_start, arena_end, got_address, stub_address;
} Load;

/* What a relocation's symbol resolves to */
typedef struct {
    uint64_t address;
    int far; /* a table symbol outside the arena, which a 32-bit displacement may not reach */
    /* its GOT slot's and stub's key: "object:symbol" for the unit's own, "@address" for a table symbol; empty for the
       GOT itself and an undefined weak symbol (address 0) */
    char key[48];
} Target;

static uint64_t align_up(uint64_t value, uint64_t alignment) { return (value + alignment - 1) / alignment * alignment; }

static uint64_t stub_size(const Load* ld) { return ld->machine == EM_AARCH64 ? SNX_ELF_VENEER_SIZE : STUB_SIZE_X86; }

#define at        snx_code_at
#define next_free snx_code_next_free

/* The unit's definitions first, then the table, as code_loader.h says. The unit's addresses are final once the
   sections are placed; before, only the key is */
static int resolve(const SnxCodeLoader* loader, const Load* ld, size_t object, size_t index, Target* t, char* error,
                   size_t error_size) {
    const SnxElfObject* o = &ld->objs[object];
    size_t def_object, def_index;
    SnxU64 address;
    int absolute, where;
    const char* name;
    memset(t, 0, sizeof(*t));
    where = snx_elf_definition(ld->objs, &ld->globals, object, index, &def_object, &def_index, error, error_size);
    if (where < 0) return -1;
    if (where == SNX_ELF_HERE) {
        t->address = snx_elf_symbol_address(ld->objs, def_object, def_index, &absolute);
        sprintf(t->key, "%lu:%lu", (unsigned long)def_object, (unsigned long)def_index);
        return 0;
    }
    name = snx_elf_symbol_name(o, &o->syms[index]);
    if (snx_code_find(loader, name, &address)) {
        t->address = address;
        t->far = t->address < ld->arena_start || t->address >= ld->arena_end;
        /* in hexadecimal by halves: unsigned long is 32 bits on LLP64 systems */
        sprintf(t->key, "@%lx:%08lx", (unsigned long)(t->address >> 32), (unsigned long)(t->address & 0xFFFFFFFFu));
        return 0;
    }
    if (!strcmp(name, "_GLOBAL_OFFSET_TABLE_")) {
        t->address = ld->got_address;
        return 0;
    }
    if (ELF64_ST_BIND(o->syms[index].st_info) == STB_WEAK) return 0;
    snprintf(error, error_size, "undefined symbol %s (referenced by %s)", name, o->input->name);
    return -1;
}

/* Whether a call to t may need a stub: on x86-64 one outside the arena (inside, rel32 reaches across it); on AArch64
   any table symbol, earlier loads' included, since the arena is larger than B's reach. The unit's own definitions are
   in reach of its calls, the unit being far smaller than 128 MiB */
static int may_need_stub(const Load* ld, const Target* t) {
    if (ld->machine == EM_AARCH64) return t->key[0] == '@';
    return t->far;
}

/* Section number section when it holds the relocations of a loaded section, which the two passes walk; else NULL */
static const Elf64_Shdr* relocations(const SnxElfObject* o, size_t section) {
    const Elf64_Shdr* s = &o->sh[section];
    if (s->sh_type != SHT_RELA || s->sh_info >= o->eh->e_shnum || o->kind[s->sh_info] < 0) return NULL;
    return s;
}

/* First pass: the GOT slots and stubs the unit needs. A stub that jumps through a GOT slot gets one */
static int scan(const SnxCodeLoader* loader, Load* ld, char* error, size_t error_size) {
    size_t i, j, k, n, slot;
    Target t;
    for (i = 0; i < ld->count; ++i) {
        const SnxElfObject* o = &ld->objs[i];
        for (j = 0; j < o->eh->e_shnum; ++j) {
            const Elf64_Shdr* s = relocations(o, j);
            const Elf64_Rela* r;
            if (o->sh[j].sh_type == SHT_REL) {
                snprintf(error, error_size, "%s: REL relocations are not supported", o->input->name);
                return -1;
            }
            if (!s) continue;
            r = (const Elf64_Rela*)(o->input->data + s->sh_offset);
            n = s->sh_size / sizeof(Elf64_Rela);
            for (k = 0; k < n; ++k) {
                unsigned type = ELF64_R_TYPE(r[k].r_info);
                if (resolve(loader, ld, i, ELF64_R_SYM(r[k].r_info), &t, error, error_size)) return -1;
                if (snx_elf_got_relocation(ld->machine, type)) {
                    if (!t.key[0]) {
                        snprintf(error, error_size, "%s: GOT reference to an undefined weak symbol", o->input->name);
                        return -1;
                    }
                    if (!snx_link_find(&ld->got, t.key, &slot)) snx_link_insert(&ld->got, t.key, ld->got_count++);
                } else if (snx_elf_call_relocation(ld->machine, type) && may_need_stub(ld, &t) &&
                           !snx_link_find(&ld->stubs, t.key, &slot)) {
                    snx_link_insert(&ld->stubs, t.key, ld->stub_count++);
                    if (t.far && !snx_link_find(&ld->got, t.key, &slot))
                        snx_link_insert(&ld->got, t.key, ld->got_count++);
                }
            }
        }
    }
    return 0;
}

/* t's GOT slot, written with its address: the slot's own address */
static uint64_t got_slot(const SnxCodeLoader* loader, const Load* ld, const Target* t) {
    size_t slot = 0;
    uint64_t address;
    snx_link_find(&ld->got, t->key, &slot);
    address = ld->got_address + 8 * (uint64_t)slot;
    snx_link_put64(at(loader, address), t->address);
    return address;
}

/* The call of relocation type at p (address P, addend A) to t, which it cannot reach, through t's stub: the stub
   written and the call to it. 0, or SNX_LINK_RANGE when t has no stub */
static int call_stub(const SnxCodeLoader* loader, const Load* ld, const Target* t, unsigned char* p, size_t room,
                     unsigned type, int64_t A, uint64_t P) {
    size_t index;
    uint64_t address, slot;
    unsigned char* s;
    if (!t->key[0] || !snx_link_find(&ld->stubs, t->key, &index)) return SNX_LINK_RANGE;
    address = ld->stub_address + stub_size(ld) * (uint64_t)index;
    s = at(loader, address);
    slot = t->far ? got_slot(loader, ld, t) : 0;
    if (ld->machine == EM_AARCH64) {
        /* the stub goes to the symbol itself, shared by every call to it: a call to an offset from it has none */
        if (A) return SNX_LINK_RANGE;
        return snx_elf_branch_veneer(p, P, s, address, t->far ? slot : t->address, t->far);
    }
    s[0] = 0xFF; /* jmp qword ptr [rip + slot - next] */
    s[1] = 0x25;
    snx_link_rel32(s + 2, slot, address + 6); /* both in the arena: in reach */
    /* PLT32's addend is the call's own, its displacement's distance from the next instruction */
    return snx_elf_relocate(ld->machine, p, room, type, address, A, P, ld->got_address, 0);
}

/* Second pass: every relocation of the unit, written into its placed sections */
static int relocate(const SnxCodeLoader* loader, const Load* ld, char* error, size_t error_size) {
    size_t i, j, k, n;
    for (i = 0; i < ld->count; ++i) {
        const SnxElfObject* o = &ld->objs[i];
        for (j = 0; j < o->eh->e_shnum; ++j) {
            const Elf64_Shdr* s = relocations(o, j);
            const Elf64_Shdr* target;
            const Elf64_Rela* r;
            if (!s) continue;
            target = &o->sh[s->sh_info];
            r = (const Elf64_Rela*)(o->input->data + s->sh_offset);
            n = s->sh_size / sizeof(Elf64_Rela);
            for (k = 0; k < n; ++k) {
                unsigned type = ELF64_R_TYPE(r[k].r_info);
                uint64_t P = o->address[s->sh_info] + r[k].r_offset, slot = 0;
                unsigned char* p;
                Target t;
                int written = SNX_LINK_OUTSIDE;
                if (resolve(loader, ld, i, ELF64_R_SYM(r[k].r_info), &t, error, error_size)) return -1;
                if (r[k].r_offset <= target->sh_size) {
                    size_t room = (size_t)(target->sh_size - r[k].r_offset);
                    p = at(loader, P);
                    if (snx_elf_got_relocation(ld->machine, type)) slot = got_slot(loader, ld, &t);
                    written = snx_elf_relocate(ld->machine, p, room, type, t.address, r[k].r_addend, P, ld->got_address,
                                               slot);
                    if (written == SNX_LINK_RANGE && snx_elf_call_relocation(ld->machine, type))
                        written = call_stub(loader, ld, &t, p, room, type, r[k].r_addend, P);
                }
                if (written == SNX_LINK_OUTSIDE) {
                    snprintf(error, error_size, "%s: relocation outside its section", o->input->name);
                    return -1;
                }
                if (written == SNX_LINK_RANGE) {
                    snprintf(error, error_size, "%s: relocation %u out of range of %s", o->input->name, type,
                             t.far ? "a symbol outside the code arena" : "its target");
                    return -1;
                }
                if (written == SNX_LINK_MISALIGNED) {
                    snprintf(error, error_size, "%s: relocation %u to a misaligned address", o->input->name, type);
                    return -1;
                }
                if (written) {
                    snprintf(error, error_size, "%s: unsupported relocation type %u", o->input->name, type);
                    return -1;
                }
            }
        }
    }
    return 0;
}

int snx_code_load(SnxCodeLoader* loader, const SnxLinkInput* inputs, size_t count, const char* const* names,
                  size_t name_count, SnxU64* addresses, char* error, size_t error_size) {
    static const char* const region_names[REGIONS] = {"code", "read-only data", "data"};
    Load ld;
    uint64_t kind_size[SNX_ELF_KINDS], kind_align[SNX_ELF_KINDS], kind_address[SNX_ELF_KINDS];
    uint64_t start[REGIONS], end[REGIONS], limit;
    size_t i, j, found;
    int rc = -1, absolute;
    if (loader->machine == SNX_CODE_COFF_X64)
        return snx_code_load_coff(loader, inputs, count, names, name_count, addresses, error, error_size);
    memset(&ld, 0, sizeof(ld));
    ld.count = count;
    ld.machine = loader->machine;
    ld.arena_start = (uint64_t)(size_t)loader->base;
    ld.arena_end = ld.arena_start + loader->reserved;
    ld.objs = (SnxElfObject*)calloc(count ? count : 1, sizeof(SnxElfObject));
    if (!ld.objs) {
        snprintf(error, error_size, "out of memory");
        return -1;
    }
    for (i = 0; i < count; ++i)
        if (snx_elf_parse(&ld.objs[i], &inputs[i], &ld.machine, error, error_size)) goto done;
    if (snx_elf_collect_globals(ld.objs, count, &loader->symbols, &ld.globals, error, error_size)) goto done;
    for (i = 0; i < name_count; ++i)
        if (!snx_link_find(&ld.globals, names[i], &found) && !snx_link_find(&loader->symbols, names[i], &found)) {
            snprintf(error, error_size, "symbol %s is not defined", names[i]);
            goto done;
        }
    if (scan(loader, &ld, error, error_size)) goto done;
    /* the unit's parts, each at its region's next free byte: [code, stubs] [read-only data, GOT] [data, bss] */
    snx_elf_layout(ld.objs, count, kind_size, kind_align);
    start[CODE] =
        align_up(next_free(loader, CODE), kind_align[SNX_ELF_TEXT] > CODE_LINE ? kind_align[SNX_ELF_TEXT] : CODE_LINE);
    kind_address[SNX_ELF_TEXT] = start[CODE];
    ld.stub_address = align_up(start[CODE] + kind_size[SNX_ELF_TEXT], 8);
    end[CODE] = ld.stub_address + stub_size(&ld) * (uint64_t)ld.stub_count;
    start[RODATA] = align_up(next_free(loader, RODATA), kind_align[SNX_ELF_RODATA]);
    kind_address[SNX_ELF_RODATA] = start[RODATA];
    ld.got_address = align_up(start[RODATA] + kind_size[SNX_ELF_RODATA], 8);
    end[RODATA] = ld.got_address + 8 * (uint64_t)ld.got_count;
    start[DATA] = align_up(next_free(loader, DATA), kind_align[SNX_ELF_DATA]);
    kind_address[SNX_ELF_DATA] = start[DATA];
    kind_address[SNX_ELF_BSS] = align_up(start[DATA] + kind_size[SNX_ELF_DATA], kind_align[SNX_ELF_BSS]);
    end[DATA] = kind_address[SNX_ELF_BSS] + kind_size[SNX_ELF_BSS];
    for (i = 0; i < REGIONS; ++i) {
        limit = ld.arena_start + loader->region[i].start + loader->region[i].size;
        if (end[i] > limit) {
            snprintf(error, error_size, "the code arena's %s is full (%lu KiB)", region_names[i],
                     (unsigned long)(loader->region[i].size >> 10));
            goto done;
        }
    }
    if (snx_code_commit(loader, end[DATA], error, error_size)) goto done;
    /* Written through the read-write views, from each region's first free byte: a failed load leaves its bytes for the
       next to write over, since no address of theirs went out and nothing runs there. Between functions and in unused
       stubs, an instruction that traps: int3 on x86-64, UDF #0 (all zero bits) on AArch64 */
    memset(at(loader, next_free(loader, CODE)), ld.machine == EM_AARCH64 ? 0 : 0xCC,
           (size_t)(end[CODE] - next_free(loader, CODE)));
    memset(at(loader, next_free(loader, RODATA)), 0, (size_t)(end[RODATA] - next_free(loader, RODATA)));
    memset(at(loader, next_free(loader, DATA)), 0, (size_t)(end[DATA] - next_free(loader, DATA)));
    for (i = 0; i < count; ++i) {
        SnxElfObject* o = &ld.objs[i];
        for (j = 0; j < o->eh->e_shnum; ++j) {
            if (o->kind[j] < 0) continue;
            o->address[j] = kind_address[o->kind[j]] + o->position[j];
            if (o->kind[j] != SNX_ELF_BSS && o->sh[j].sh_size)
                memcpy(at(loader, o->address[j]), o->input->data + o->sh[j].sh_offset, o->sh[j].sh_size);
        }
    }
    if (relocate(loader, &ld, error, error_size)) goto done;
    snx_code_publish(loader, start[CODE], end[CODE]);
    for (i = 0; i < name_count; ++i)
        if (snx_link_find(&ld.globals, names[i], &found))
            addresses[i] = snx_elf_symbol_address(ld.objs, found >> 32, found & 0xFFFFFFFFu, &absolute);
        else
            snx_code_lookup(loader, names[i], &addresses[i]);
    for (i = 0; i < ld.globals.capacity; ++i)
        if (ld.globals.keys[i])
            snx_link_insert(&loader->symbols, ld.globals.keys[i],
                            (size_t)snx_elf_symbol_address(ld.objs, ld.globals.values[i] >> 32,
                                                           ld.globals.values[i] & 0xFFFFFFFFu, &absolute));
    /* a region the unit put nothing into keeps its next free byte, so an empty load uses nothing */
    for (i = 0; i < REGIONS; ++i)
        if (end[i] > start[i]) loader->region[i].used = (size_t)(end[i] - ld.arena_start - loader->region[i].start);
    rc = 0;
done:
    for (i = 0; i < count; ++i) snx_elf_free(&ld.objs[i]);
    free(ld.objs);
    snx_link_names_free(&ld.globals);
    snx_link_names_free(&ld.got);
    snx_link_names_free(&ld.stubs);
    return rc;
}
#else
int snx_code_load(SnxCodeLoader* loader, const SnxLinkInput* inputs, size_t count, const char* const* names,
                  size_t name_count, SnxU64* addresses, char* error, size_t error_size) {
    if (loader->machine == SNX_CODE_COFF_X64)
        return snx_code_load_coff(loader, inputs, count, names, name_count, addresses, error, error_size);
    snprintf(error, error_size, "the JIT loader reads COFF objects on Windows");
    return -1;
}
#endif
