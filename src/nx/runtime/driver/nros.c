/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/runtime/driver/nros.h"
#include "common/utilities/endian.h"
#include "common/platform/file.h"
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* a .nro file larger than this is no game's NRO: the recompile stops at it, with its size */
#define NRO_MAX 0x40000000ull

static void hex(const unsigned char* hash, char* out) {
    static const char digits[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < 32; ++i) {
        out[2 * i] = digits[hash[i] >> 4];
        out[2 * i + 1] = digits[hash[i] & 15];
    }
    out[64] = 0;
}

/* data (size bytes, owned: freed when it is left out) as the NRO name, added to set when ldr:ro would map it: its
   header's sizes are whole pages, as the service asks of the ranges it is given. 0, or nonzero, said why */
static int add(SnxNroSet* set, const char* name, unsigned char* data, size_t size, int quiet) {
    SnxNroFile* f;
    SnxU32 image, bss;
    const char* why = NULL;
    if (size < 0x80 || memcmp(data + 0x10, "NRO0", 4))
        why = "it is not an NRO";
    else if (snx_ld_le32(data + 0x1c) & 2)
        why = "it is compressed";
    else if ((image = snx_ld_le32(data + 0x18)) > size || image < 0x1000)
        why = "its image is not all in the file";
    else if ((image & 0xFFF) || ((bss = snx_ld_le32(data + 0x38)) & 0xFFF))
        why = "its image or its BSS is not whole pages, which ldr:ro maps none of";
    if (why) {
        if (!quiet) printf("NRO %s: %s: no code is made for it\n", name, why);
        free(data);
        return 0;
    }
    set->files = (SnxNroFile*)snx_realloc(set->files, (set->file_count + 1) * sizeof(*set->files));
    f = &set->files[set->file_count++];
    f->name = snx_strdup(name);
    f->data = data;
    f->size = image;
    if (!EVP_Digest(data, image, f->hash, NULL, EVP_sha256(), NULL))
        return snx_error("NRO %s: its SHA-256 could not be made (out of memory)", name);
    return 0;
}

static int folder(SnxNroSet* set, const char* path, int quiet, unsigned depth) {
    char **names = NULL, *child;
    size_t count = 0, i, n, size;
    unsigned char* data;
    int rc = 0;
    if (depth > 32 || snx_list_dir(path, &names, &count)) return snx_error("cannot list %s", path);
    for (i = 0; !rc && i < count; ++i) {
        child = snx_path_join(path, names[i]);
        n = strlen(names[i]);
        if (snx_is_dir(child) && !snx_is_link(child))
            rc = folder(set, child, quiet, depth + 1);
        else if (n > 4 && names[i][n - 4] == '.' && (names[i][n - 3] | 0x20) == 'n' &&
                 (names[i][n - 2] | 0x20) == 'r' && (names[i][n - 1] | 0x20) == 'o') {
            if (snx_file_size(child) > NRO_MAX)
                rc = snx_error("%s is %llu bytes, and an NRO is read up to %llu", child,
                               (unsigned long long)snx_file_size(child), NRO_MAX);
            else if (!(data = snx_read_file(child, &size)))
                rc = snx_error("cannot read %s", child);
            else
                rc = add(set, names[i], data, size, quiet);
        }
        free(child);
    }
    snx_free_strings(names, count);
    return rc;
}
int snx_nros_folder(SnxNroSet* set, const char* romfs, int quiet) { return folder(set, romfs, quiet, 0); }

typedef struct {
    SnxNroSet* set;
    SnxHacFs* fs;
    int quiet;
} DumpWalk;
static int dump_file(void* context, const char* name, SnxU64 offset, SnxU64 size) {
    DumpWalk* w = (DumpWalk*)context;
    unsigned char* data;
    if (size > NRO_MAX)
        return snx_error("the RomFS's %s is %llu bytes, and an NRO is read up to %llu", name, (unsigned long long)size,
                         NRO_MAX);
    data = (unsigned char*)snx_alloc(size ? (size_t)size : 1);
    if (snx_hac_fs_read_romfs(w->fs, offset, data, (size_t)size)) {
        free(data);
        return snx_error("cannot read the RomFS's %s", name);
    }
    return add(w->set, name, data, (size_t)size, w->quiet);
}
int snx_nros_dump(SnxNroSet* set, SnxHacFs* fs, int quiet) {
    DumpWalk w;
    int rc;
    w.set = set;
    w.fs = fs;
    w.quiet = quiet;
    rc = snx_romfs_each(fs, ".nro", dump_file, &w);
    if (rc < 0) return snx_error("the RomFS's file table cannot be read: its NROs are not found");
    return rc;
}

/* the file of a placed NRO: the first found with its hash */
static const SnxNroFile* file_of(const SnxNroSet* set, const SnxNroSlot* place) {
    size_t k;
    for (k = 0; k < set->file_count; ++k)
        if (!memcmp(set->files[k].hash, place->hash, 32)) return &set->files[k];
    return NULL;
}
int snx_nros_place(SnxNroSet* set, const SnxProgram* program, int quiet) {
    char error[512];
    SnxU64 after = 0, words = 0;
    size_t i;
    if (!set->file_count) return 0;
    for (i = 0; i < program->module_count; ++i)
        if (program->modules[i].base + program->modules[i].size > after)
            after = program->modules[i].base + program->modules[i].size;
    set->places = (SnxNroSlot*)snx_alloc(set->file_count * sizeof(*set->places));
    for (i = 0; i < set->file_count; ++i) {
        const SnxNroFile* f = &set->files[i];
        memcpy(set->places[i].hash, f->hash, 32);
        set->places[i].base = 0;
        set->places[i].size = (SnxU64)f->size + snx_ld_le32(f->data + 0x38);
    }
    set->count = set->file_count;
    if (snx_nro_slots_place(set->places, &set->count, after, &set->end, error, sizeof error)) {
        set->count = 0;
        return snx_error("%s", error);
    }
    for (i = 0; i < set->count; ++i) words += snx_ld_le32(file_of(set, &set->places[i])->data + 0x24) / 4;
    if (!quiet)
        printf("NROs: %zu in the RomFS (%zu files), %llu instructions of text, at 0x%llX-0x%llX\n", set->count,
               set->file_count, (unsigned long long)words, (unsigned long long)set->places[0].base,
               (unsigned long long)set->end);
    return 0;
}

/* A function a module of the game exports, by its name: what an NRO's import of that name is bound to when one
   module alone defines it. Functions a hook names are left out, their calls go through their dispatch slots */
typedef struct {
    const char* name;
    SnxU64 address;
} Export;
static int export_order(const void* a, const void* b) {
    return strcmp(((const Export*)a)->name, ((const Export*)b)->name);
}
/* the game's exports by name, each name once: one two modules define is left out, as the game's linker decides
   between them. The names are the program's */
static Export* exports_of(const SnxProgram* program, size_t* count) {
    Export* all;
    size_t m, k, n = 0, kept = 0, i, j;
    for (m = 0; m < program->module_count; ++m) n += program->modules[m].symbol_count;
    all = (Export*)snx_alloc((n ? n : 1) * sizeof(*all));
    n = 0;
    for (m = 0; m < program->module_count; ++m) {
        const SnxModuleImage* module = &program->modules[m];
        for (k = 0; k < module->symbol_count; ++k) {
            const SnxSymbol* s = &module->symbols[k];
            size_t lo = 0, hi = module->routed_count;
            if (s->type != 2 /* STT_FUNC */ || (s->value & 3) || s->value < module->text_off ||
                s->value - module->text_off >= module->text_size)
                continue;
            while (lo < hi) {
                size_t middle = lo + (hi - lo) / 2;
                if (module->routed[middle] < s->value)
                    lo = middle + 1;
                else
                    hi = middle;
            }
            if (lo < module->routed_count && module->routed[lo] == s->value) continue;
            all[n].name = s->name;
            all[n++].address = module->base + s->value;
        }
    }
    qsort(all, n, sizeof(*all), export_order);
    for (i = 0; i < n; i = j) {
        for (j = i + 1; j < n && !strcmp(all[i].name, all[j].name); ++j) continue;
        if (j == i + 1) all[kept++] = all[i];
    }
    *count = kept;
    return all;
}

/* The NRO's bound branches (module.h): each PLT stub of its text, adrp x16 / ldr x17,[x16,#slot] / add x16 / br x17,
   whose slot's symbol names a function of the NRO itself or one the game's modules define once. *sites: the BL and B
   instructions that go to those stubs. Owned by the caller */
static SnxBoundBranch* bound_of(const SnxModuleImage* nro, const Export* exports, size_t export_count, size_t* count,
                                size_t* own, size_t* sites, char* error, size_t error_size) {
    SnxPltSlot* slots = NULL;
    SnxBoundBranch* bound = NULL;
    size_t slot_count = 0, n = 0;
    SnxU64 pc, end = nro->text_off + nro->text_size;
    *count = *own = *sites = 0;
    if (snx_module_plt(nro, &slots, &slot_count, error, error_size)) return NULL;
    if (slot_count) bound = (SnxBoundBranch*)snx_alloc(slot_count * sizeof(*bound));
    for (pc = nro->text_off; slot_count && n < slot_count && end - pc >= 16; pc += 4) {
        SnxU32 adrp = snx_ld_le32(nro->image + pc), ldr = snx_ld_le32(nro->image + pc + 4);
        SnxU64 page, slot, target = 0;
        size_t lo = 0, hi = slot_count;
        int game = 0;
        if (snx_ld_le32(nro->image + pc + 12) != 0xD61F0220UL || (adrp & 0x9F00001FUL) != 0x90000010UL ||
            (ldr & 0xFFC003FFUL) != 0xF9400211UL || (snx_ld_le32(nro->image + pc + 8) & 0xFFC003FFUL) != 0x91000210UL)
            continue;
        page = ((SnxU64)((adrp >> 5) & 0x7FFFF) << 2 | ((adrp >> 29) & 3)) << 12;
        if (page & ((SnxU64)1 << 32)) page |= ~(SnxU64)0 << 33; /* a page before this one */
        slot = (pc & ~(SnxU64)0xFFF) + page + (SnxU64)((ldr >> 10) & 0xFFF) * 8;
        while (lo < hi) {
            size_t middle = lo + (hi - lo) / 2;
            if (slots[middle].slot < slot)
                lo = middle + 1;
            else
                hi = middle;
        }
        if (lo == slot_count || slots[lo].slot != slot) continue;
        if (slots[lo].value) { /* the NRO's own function */
            if (!(slots[lo].value & 3) && slots[lo].value >= nro->text_off && slots[lo].value < end)
                target = nro->base + slots[lo].value;
        } else {
            Export key;
            const Export* found;
            key.name = slots[lo].name;
            found = (const Export*)bsearch(&key, exports, export_count, sizeof(*exports), export_order);
            if (found) target = found->address, game = 1;
        }
        if (!target) continue;
        bound[n].at = nro->base + pc + 12;
        bound[n].target = target;
        bound[n].game = game;
        *own += !game;
        ++n;
    }
    /* the calls of them: every BL and B of the text whose target is such a stub (its BR 12 bytes on) */
    for (pc = nro->text_off; n && pc < end; pc += 4) {
        SnxU32 word = snx_ld_le32(nro->image + pc);
        SnxU64 to, lo = 0, hi = n;
        if ((word & 0x7C000000UL) != 0x14000000UL) continue;
        to = pc + ((SnxU64)(word & 0x3FFFFFFUL) << 2);
        if (word & 0x2000000UL) to -= (SnxU64)1 << 28;
        to += nro->base + 12;
        while (lo < hi) {
            SnxU64 middle = lo + (hi - lo) / 2;
            if (bound[middle].at < to)
                lo = middle + 1;
            else
                hi = middle;
        }
        *sites += lo < n && bound[lo].at == to;
    }
    free(slots);
    *count = n;
    return bound;
}

/* a module of an NRO, by its name: 64 hex digits and the suffix, or that and .tmp (a write that did not end) */
static int module_name(const char* name, const char* suffix) {
    size_t i, n = strlen(name), s = strlen(suffix);
    for (i = 0; i < 64 && i < n; ++i)
        if (!((name[i] >= '0' && name[i] <= '9') || (name[i] >= 'a' && name[i] <= 'f'))) return 0;
    if (i < 64 || strncmp(name + 64, suffix, s)) return 0;
    return !name[64 + s] || !strcmp(name + 64 + s, ".tmp");
}

int snx_nros_build(const SnxNroSet* set, const SnxProgram* program, const SnxCodegenOptions* gen,
                   const SnxLlvmBuild* build, const char* folder, int windows) {
    const char* suffix = windows ? ".dll" : ".so";
    char **names = NULL, text[65], error[1024];
    size_t count = 0, i, k, found, made = 0, code = 0, export_count = 0, stubs = 0, stubs_own = 0, sites = 0;
    Export* exports = set->count ? exports_of(program, &export_count) : NULL;
    double start, all = snx_seconds();
    int rc = 0;
    if (snx_is_link(folder)) return snx_error("%s is a link, and the NROs' code goes in a folder", folder);
    if (set->count && snx_mkdirs(folder)) return snx_error("cannot create %s", folder);
    for (i = 0; !rc && i < set->count; ++i) {
        const SnxNroSlot* place = &set->places[i];
        const SnxNroFile* file = file_of(set, place);
        SnxProgram nro;
        SnxCodegenOptions options = *gen;
        SnxLlvmBuild llvm = *build;
        SnxBoundBranch* bound;
        size_t bound_count, own, called;
        char *name, *output;
        hex(place->hash, text);
        if (snx_load_nro_at(file->data, file->size, file->name, place->base, &program->npdm, &nro, error,
                            sizeof error)) {
            /* its place in the table stays: the runtime maps it there and finds no module, so its JIT runs it */
            printf("NRO %s: %s: no code is made for it\n", file->name, error);
            continue;
        }
        name = snx_format("%s%s", text, suffix);
        output = snx_path_join(folder, name);
        /* its PLT's calls of its own functions and of the game's, made direct (module.h's SnxBoundBranch) */
        error[0] = 0;
        bound = NULL;
        bound_count = own = called = 0;
        if (!gen->hook_direct_calls) /* every call through its slot: none bound */
            bound = bound_of(&nro.modules[0], exports, export_count, &bound_count, &own, &called, error, sizeof error);
        if (!bound && error[0])
            printf("NRO %s: %s: its PLT's calls go through the dispatch table\n", file->name, error);
        options.bound = bound;
        options.bound_count = bound_count;
        options.hints = NULL;
        options.hint_count = 0;
        options.jit_cache = NULL;
        options.nros = NULL;
        options.nro_count = 0;
        options.quiet = 1;
        found = 0;
        llvm.output = output;
        llvm.quiet = 1;
        llvm.keep_ir = NULL;
        llvm.wrappers = NULL;
        llvm.found = &found;
        llvm.layout = NULL;
        llvm.layout_count = 0;
        start = snx_seconds();
        rc = snx_build_module_llvm(&nro, &options, &llvm);
        if (rc)
            snx_error("the code of the NRO %s (%s) was not made", file->name, name);
        else if (!build->quiet)
            printf("  NRO %zu/%zu: %s at 0x%llX, %zu of %u instructions, %s (%.1f s)\n", i + 1, set->count, file->name,
                   (unsigned long long)place->base, found, (unsigned)(nro.modules[0].text_size / 4), name,
                   snx_seconds() - start);
        made += found;
        code += nro.modules[0].text_size / 4;
        stubs += bound_count;
        stubs_own += own;
        sites += called;
        fflush(stdout);
        free(bound);
        free(name);
        free(output);
        snx_free_program(&nro);
    }
    /* the modules of NROs this recompile has not: an earlier one's, of another version of the game or another layout */
    if (!rc && snx_is_dir(folder) && !snx_list_dir(folder, &names, &count)) {
        for (i = 0; !rc && i < count; ++i) {
            char* path;
            int ours = 0;
            if (!module_name(names[i], suffix)) continue;
            for (k = 0; k < set->count && !ours; ++k) {
                hex(set->places[k].hash, text);
                ours = !strncmp(names[i], text, 64) && !names[i][64 + strlen(suffix)];
            }
            if (ours) continue;
            path = snx_path_join(folder, names[i]);
            if (snx_remove_tree(path))
                rc = snx_error("cannot remove %s, the code of an NRO the game has no more", path);
            free(path);
        }
        snx_free_strings(names, count);
    }
    if (!rc && set->count && !build->quiet)
        printf("NROs: the code of %zu, %zu of %zu instructions, in %s (%.1f s); %zu calls through %zu PLT stubs made "
               "direct, %zu stubs of the NROs' own functions and %zu of the game's\n",
               set->count, made, code, folder, snx_seconds() - all, sites, stubs, stubs_own, stubs - stubs_own);
    free(exports);
    return rc;
}

void snx_nros_free(SnxNroSet* set) {
    size_t i;
    for (i = 0; i < set->file_count; ++i) {
        free(set->files[i].name);
        free(set->files[i].data);
    }
    free(set->files);
    free(set->places);
    memset(set, 0, sizeof(*set));
}
