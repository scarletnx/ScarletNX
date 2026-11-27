/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The JIT loader's objects as GDB reads them (gdb_jit.c), through GDB's own descriptor: nothing while registration is
   off, or for another machine's loader; once on, each load's object in the list, newest first with its links both
   ways, a copy of the object but for the loaded sections' sh_addr, at which the object's symbols come out at the
   addresses the load gave them; a loader's free takes out its entries alone, another JIT's among them kept; and GDB's
   breakpoint called once per change, with the entry and the action. Skipped on Windows (no registration) and where
   the loader cannot run what it loads */
#include "common/cpu/recompiler/loader/code_loader.h"
#include "common/cpu/recompiler/loader/gdb_jit.h"
#include "common/cpu/recompiler/mach/native.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
int main(void) {
    printf("gdb jit: skipped (nothing is registered on Windows)\n");
    return 77;
}
#else
#include "common/cpu/recompiler/object/elf_object.h" /* Elf64_* on every ELF host and macOS */

/* GDB's, as gdb_jit.c declares them */
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
extern struct jit_descriptor __jit_debug_descriptor;

/* GDB's breakpoint, this program's over gdb_jit.c's weak one: what each call saw */
static struct {
    int calls;
    uint32_t action;
    uintptr_t entry;
} seen;
void __jit_debug_register_code(void);
void __jit_debug_register_code(void) {
    ++seen.calls;
    seen.action = __jit_debug_descriptor.action_flag;
    seen.entry = (uintptr_t)__jit_debug_descriptor.relevant_entry;
}

static int failures;
#define CHECK(cond, ...)                                                                                               \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            fprintf(stderr, "line %d: ", __LINE__);                                                                    \
            fprintf(stderr, __VA_ARGS__);                                                                              \
            fputc('\n', stderr);                                                                                       \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)

static const char* const names[2] = {"snx_test_fn", "snx_test_value"};

/* snx_test_fn and snx_test_value, code and data with a relocation between them, in an object of the host's native back
   end (machine 0) or of machine's (gdb_jit_object.c: its ELF builder's names clash with elf_object.h's) */
int gdb_jit_object(const char* const names[2], unsigned machine, unsigned pointer, unsigned char** data, size_t* size,
                   char* error, size_t error_size);
static int object(unsigned machine, unsigned pointer, unsigned char** data, size_t* size, char* error,
                  size_t error_size) {
    return gdb_jit_object(names, machine, pointer, data, size, error, error_size);
}
static int load(SnxCodeLoader* loader, const unsigned char* data, size_t size, SnxU64* addresses) {
    char error[1024];
    SnxLinkInput input;
    input.data = data;
    input.size = size;
    input.name = "gdb_jit";
    if (snx_code_load(loader, &input, 1, names, 2, addresses, error, sizeof(error))) {
        fprintf(stderr, "load: %s\n", error);
        return -1;
    }
    return 0;
}

static Elf64_Shdr section(const unsigned char* o, size_t i) {
    Elf64_Ehdr eh;
    Elf64_Shdr sh;
    memcpy(&eh, o, sizeof(eh));
    memcpy(&sh, o + eh.e_shoff + i * sizeof(sh), sizeof(sh));
    return sh;
}

/* entry is the object at data, with only sh_addr changed, and its names at addresses */
static void check_entry(const struct jit_code_entry* entry, const unsigned char* data, size_t size,
                        const SnxU64* addresses) {
    const unsigned char* o = (const unsigned char*)entry->symfile_addr;
    unsigned char* expect;
    Elf64_Ehdr eh;
    size_t i, k, found = 0;
    CHECK(entry->symfile_size == size, "the entry has %llu bytes, the object %zu",
          (unsigned long long)entry->symfile_size, size);
    if (entry->symfile_size != size) return;
    memcpy(&eh, o, sizeof(eh));
    expect = (unsigned char*)malloc(size);
    if (!expect) abort();
    memcpy(expect, data, size);
    for (i = 0; i < eh.e_shnum; ++i) {
        Elf64_Shdr sh = section(o, i), was = section(data, i);
        CHECK(sh.sh_addr == was.sh_addr || (sh.sh_flags & SHF_ALLOC), "section %zu, not loaded, has sh_addr %llx", i,
              (unsigned long long)sh.sh_addr);
        memcpy(expect + eh.e_shoff + i * sizeof(sh) + offsetof(Elf64_Shdr, sh_addr), &sh.sh_addr, sizeof(sh.sh_addr));
    }
    CHECK(!memcmp(expect, o, size), "the entry's object differs from the load's in more than sh_addr");
    free(expect);
    /* an ET_REL symbol is at its section's address plus st_value, as GDB takes it */
    for (i = 0; i < eh.e_shnum; ++i) {
        Elf64_Shdr symtab = section(o, i), strtab;
        if (symtab.sh_type != SHT_SYMTAB) continue;
        strtab = section(o, symtab.sh_link);
        for (k = 1; k < symtab.sh_size / sizeof(Elf64_Sym); ++k) {
            Elf64_Sym s;
            size_t n;
            memcpy(&s, o + symtab.sh_offset + k * sizeof(s), sizeof(s));
            for (n = 0; n < 2; ++n)
                if (!strcmp((const char*)o + strtab.sh_offset + s.st_name, names[n])) {
                    uint64_t at = section(o, s.st_shndx).sh_addr + s.st_value;
                    CHECK(at == addresses[n], "%s comes out at %llx, loaded at %llx", names[n], (unsigned long long)at,
                          (unsigned long long)addresses[n]);
                    ++found;
                }
        }
    }
    CHECK(found == 2, "%zu of the two names in the entry's symbol table", found);
}

/* another machine's object in its own loader: nothing registered */
static void check_other_machine(void) {
#if defined(__x86_64__)
    unsigned machine = 183, pointer = 257; /* AArch64's, R_AARCH64_ABS64 */
#elif defined(__aarch64__)
    unsigned machine = 62, pointer = 1; /* x86-64's, R_X86_64_64 */
#else
    unsigned machine = 0, pointer = 0;
#endif
    char error[1024];
    unsigned char* data;
    size_t size;
    SnxU64 addresses[2];
    SnxCodeLoader* loader;
    if (!machine) {
        printf("gdb jit: no other machine named for this host, its loader unchecked\n");
        return;
    }
    if (object(machine, pointer, &data, &size, error, sizeof(error))) {
        fprintf(stderr, "%s\n", error);
        ++failures;
        return;
    }
    loader = snx_code_loader_new_machine((size_t)1 << 24, machine, error, sizeof(error));
    CHECK(loader != NULL, "loader for machine %u: %s", machine, error);
    if (loader) {
        int calls = seen.calls;
        struct jit_code_entry* first = __jit_debug_descriptor.first_entry;
        if (!load(loader, data, size, addresses)) {
            CHECK(seen.calls == calls && __jit_debug_descriptor.first_entry == first,
                  "another machine's load was registered");
        } else
            ++failures;
        snx_code_loader_free(loader);
    }
    free(data);
}

int main(void) {
    char error[1024];
    unsigned char* data;
    size_t size;
    SnxU64 a_at[2], b_at[2], off_at[2];
    SnxCodeLoader *off, *a, *b;
    struct jit_code_entry *ea, *eb, foreign;
    uintptr_t ea_id, eb_id;
    if (!snx_native_available()) {
        printf("gdb jit: skipped (this host has no native back end, %s, to make the object)\n", snx_native_name());
        return 77;
    }
    if (object(0, 0, &data, &size, error, sizeof(error))) {
        fprintf(stderr, "%s\n", error);
        return 1;
    }
    CHECK(__jit_debug_descriptor.version == 1, "descriptor version %u", __jit_debug_descriptor.version);

    /* off: a load and a free register and notify nothing */
    off = snx_code_loader_new((size_t)1 << 24, error, sizeof(error));
    if (!off) {
        printf("gdb jit: skipped (the loader runs nothing here: %s)\n", error);
        return 77;
    }
    if (load(off, data, size, off_at)) return 1;
    CHECK(!__jit_debug_descriptor.first_entry && !seen.calls, "a load registered with registration off");
    snx_code_loader_free(off);
    CHECK(!seen.calls, "a free notified with registration off");

    snx_gdb_jit_enable();
    check_other_machine();
    a = snx_code_loader_new((size_t)1 << 24, error, sizeof(error));
    b = snx_code_loader_new((size_t)1 << 24, error, sizeof(error));
    if (!a || !b) {
        fprintf(stderr, "loader: %s\n", error);
        return 1;
    }

    /* A's load, then another JIT's entry linked in front as it would, then B's load: B, foreign, A */
    if (load(a, data, size, a_at)) return 1;
    ea = __jit_debug_descriptor.first_entry;
    CHECK(ea && seen.calls == 1 && seen.action == JIT_REGISTER_FN && seen.entry == (uintptr_t)ea,
          "A's load: %d calls, action %u", seen.calls, seen.action);
    if (!ea) return 1;
    CHECK(!ea->prev_entry && !ea->next_entry, "A's entry is not alone");
    check_entry(ea, data, size, a_at);
    memset(&foreign, 0, sizeof(foreign));
    foreign.next_entry = ea;
    ea->prev_entry = &foreign;
    __jit_debug_descriptor.first_entry = &foreign;
    if (load(b, data, size, b_at)) return 1;
    eb = __jit_debug_descriptor.first_entry;
    CHECK(eb && eb != &foreign && seen.calls == 2 && seen.action == JIT_REGISTER_FN && seen.entry == (uintptr_t)eb,
          "B's load: %d calls, action %u", seen.calls, seen.action);
    if (!eb || eb == &foreign) return 1;
    CHECK(a_at[0] != b_at[0], "both loads at %llx", (unsigned long long)a_at[0]);
    CHECK(!eb->prev_entry && eb->next_entry == &foreign && foreign.prev_entry == eb && foreign.next_entry == ea &&
              ea->prev_entry == &foreign && !ea->next_entry,
          "the list is not B, foreign, A both ways");
    check_entry(eb, data, size, b_at);
    CHECK(__jit_debug_descriptor.action_flag == JIT_NOACTION && !__jit_debug_descriptor.relevant_entry,
          "the descriptor still names a change after it");

    /* A's free takes A's entry alone, then B's B's: the foreign one stays */
    ea_id = (uintptr_t)ea;
    snx_code_loader_free(a);
    CHECK(seen.calls == 3 && seen.action == JIT_UNREGISTER_FN && seen.entry == ea_id, "A's free: %d calls, action %u",
          seen.calls, seen.action);
    CHECK(__jit_debug_descriptor.first_entry == eb && eb->next_entry == &foreign && foreign.prev_entry == eb &&
              !foreign.next_entry,
          "after A's free the list is not B, foreign");
    eb_id = (uintptr_t)eb;
    snx_code_loader_free(b);
    CHECK(seen.calls == 4 && seen.action == JIT_UNREGISTER_FN && seen.entry == eb_id, "B's free: %d calls, action %u",
          seen.calls, seen.action);
    CHECK(__jit_debug_descriptor.first_entry == &foreign && !foreign.prev_entry && !foreign.next_entry,
          "after B's free the list is not the foreign entry alone");
    CHECK(__jit_debug_descriptor.action_flag == JIT_NOACTION && !__jit_debug_descriptor.relevant_entry,
          "the descriptor still names a change after it");
    __jit_debug_descriptor.first_entry = NULL;
    free(data);
    if (failures) return 1;
    printf("gdb jit: passed\n");
    return 0;
}
#endif
