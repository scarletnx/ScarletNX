/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nds/cpu/recomp.h"
#include "nds/aot/nds_module.h"
#include "common/cpu/arm/exec.h"
#include "common/cpu/arm/llvm.h"
#include "common/platform/dl.h"
#include "common/utilities/common.h"
#include "common/utilities/endian.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A block and the bytes it was compiled from. The game's code is in RAM, and what an address holds changes (overlays
   load over one another, a game copies code about): a block runs only while memory holds its bytes, which each
   dispatch compares. Blocks of other code at the same address stay, each found again when its code comes back */
typedef struct {
    uint32_t key;  /* pc | 1 for Thumb */
    uint32_t next; /* another block at the same address, of other code; 0 for none */
    SnxArmBlock fn;
    const unsigned char* code; /* the bytes: a region's, or a copy */
    uint16_t bytes;
    uint8_t units;  /* instructions, Thumb halfwords */
    int8_t source;  /* 0 the module's, 1 compiled this run from the image's code (a hint), 2 from other code */
    uint32_t region; /* the image's region it is of */
} Entry;

#define FAST_SLOTS 0x10000u
struct NdsRecomp {
    const NdsRom* rom;
    void* library;
    const NdsModule* module;
    struct SnxArmExec* exec;
    Entry* entries; /* from 1 */
    uint32_t count, capacity;
    uint32_t* slots; /* an open addressing index of each key's first entry, slot_count a power of two */
    uint32_t slot_count;
    uint32_t fast[FAST_SLOTS]; /* the entry a key last ran, by the key's low bits */
    uint64_t compiled, hinted, from_module;
};

static const SnxArmCodegenOptions codegen = {SNX_ARM_CODEGEN_OPTIONS_ABI, SNX_ARM_PROFILE_ARM946E, SNX_ARM_MEMORY_FLAT,
                                             0, 0};

static bool module_fits(const NdsModule* m, const NdsRom* rom) {
    if (!m || m->magic != NDS_MODULE_MAGIC || m->abi != NDS_MODULE_ABI || m->arm_abi != SNX_ARM_ABI ||
        m->cpu_size != sizeof(SnxArmCPU) || m->pointer_size != sizeof(void*) ||
        m->profile != SNX_ARM_PROFILE_ARM946E || m->region_count != rom->region_count || m->identity != rom->identity)
        return false;
    return true;
}

bool nds_module_current(const char* path, const NdsRom* rom) {
    /* the descriptor's first fields, found in the file by its magic: the module is loaded once, when it runs */
    size_t size = 0;
    unsigned char* file = snx_is_file(path) ? snx_read_file(path, &size) : NULL;
    bool current = false;
    for (size_t at = 0; file && at + sizeof(NdsModule) <= size; at += 8) {
        if (snx_ld_le64(file + at) != NDS_MODULE_MAGIC) continue;
        NdsModule m;
        memcpy(&m, file + at, sizeof m);
        m.region_count = rom->region_count == m.region_count ? m.region_count : 0;
        current = m.abi == NDS_MODULE_ABI && m.arm_abi == SNX_ARM_ABI && m.cpu_size == sizeof(SnxArmCPU) &&
                  m.pointer_size == sizeof(void*) && m.profile == SNX_ARM_PROFILE_ARM946E && m.region_count &&
                  m.identity == rom->identity;
        break;
    }
    free(file);
    return current;
}

NdsRecomp* nds_recomp_new(const NdsRom* rom, const char* module, char* note, size_t note_size) {
    NdsRecomp* r = calloc(1, sizeof *r);
    if (note_size) note[0] = 0;
    if (!r) return NULL;
    r->rom = rom;
    r->slot_count = 1u << 16;
    r->slots = calloc(r->slot_count, sizeof *r->slots);
    r->capacity = 1u << 14;
    r->entries = calloc(r->capacity, sizeof *r->entries);
    if (!r->slots || !r->entries) {
        nds_recomp_free(r);
        return NULL;
    }
    if (module && snx_is_file(module)) {
        r->library = plat_dl_open(module);
        if (!r->library)
            snprintf(note, note_size, "cannot load %s: %s", module, plat_dl_error());
        else {
            r->module = plat_dl_sym(r->library, NDS_MODULE_SYMBOL);
            if (!module_fits(r->module, rom)) {
                snprintf(note, note_size, "%s is another image's or another build's", module);
                r->module = NULL;
            }
        }
    }
    return r;
}

void nds_recomp_free(NdsRecomp* r) {
    if (!r) return;
    for (uint32_t i = 1; i <= r->count; ++i)
        if (r->entries[i].source) free((void*)r->entries[i].code);
    snx_arm_exec_free(r->exec);
    if (r->library) plat_dl_close(r->library);
    free(r->entries);
    free(r->slots);
    free(r);
}

bool nds_recomp_has_module(const NdsRecomp* r) { return r->module != NULL; }

void nds_recomp_counts(const NdsRecomp* r, uint64_t* compiled, uint64_t* hinted, uint64_t* from_module) {
    *compiled = r->compiled, *hinted = r->hinted, *from_module = r->from_module;
}

uint32_t nds_recomp_thumb(const NdsRecomp* r) {
    uint32_t thumb = 0;
    for (uint32_t i = 1; i <= r->count; ++i) thumb += r->entries[i].key & 1;
    return thumb;
}

static uint32_t slot_of(const NdsRecomp* r, uint32_t key) {
    return (uint32_t)(((uint64_t)key * 0x9E3779B97F4A7C15ull) >> 40) & (r->slot_count - 1);
}

static uint32_t* slot_for(NdsRecomp* r, uint32_t key) {
    uint32_t at = slot_of(r, key);
    while (r->slots[at] && r->entries[r->slots[at]].key != key) at = (at + 1) & (r->slot_count - 1);
    return &r->slots[at];
}

static bool grow(NdsRecomp* r) {
    if (r->count + 2 >= r->capacity) {
        Entry* grown = realloc(r->entries, (size_t)r->capacity * 2 * sizeof *grown);
        if (!grown) return false;
        r->entries = grown, r->capacity *= 2;
    }
    if ((uint64_t)r->count * 2 >= r->slot_count) {
        uint32_t count = r->slot_count * 2;
        uint32_t* slots = calloc(count, sizeof *slots);
        if (!slots) return false;
        uint32_t* old = r->slots;
        uint32_t old_count = r->slot_count;
        r->slots = slots, r->slot_count = count;
        for (uint32_t i = 0; i < old_count; ++i)
            if (old[i]) *slot_for(r, r->entries[old[i]].key) = old[i];
        free(old);
    }
    return true;
}

/* the module's block at pc of a region whose bytes memory holds there, into *e. False when it has none */
static bool from_module(NdsRecomp* r, const unsigned char* at, uint32_t pc, int thumb, Entry* e) {
    unsigned width = thumb ? 2 : 4;
    for (uint32_t i = 0; r->module && i < r->module->region_count; ++i) {
        const NdsModuleRegion* m = &r->module->regions[i];
        if (pc < m->address || pc - m->address >= m->size) continue;
        uint32_t index = (pc - m->address) / width;
        SnxArmBlock fn = thumb ? m->thumb[index] : m->arm[index];
        unsigned units = thumb ? m->thumb_units[index] : m->arm_units[index];
        if (!fn || !units || (uint64_t)(pc - m->address) + units * width > m->size) continue;
        const unsigned char* code = r->rom->regions[i].data + (pc - m->address);
        if (memcmp(at, code, units * width)) continue;
        e->fn = fn, e->code = code, e->bytes = (uint16_t)(units * width), e->units = (uint8_t)units;
        e->source = 0, e->region = i;
        return true;
    }
    return false;
}

static bool compile(NdsRecomp* r, const unsigned char* at, uint32_t pc, int thumb, Entry* e, char* error,
                    size_t error_size) {
    size_t consumed = 0;
    if (!r->exec && !(r->exec = snx_arm_exec_new(error, error_size))) return false;
    if (thumb) {
        uint16_t words[NDS_BLOCK_UNITS];
        for (unsigned i = 0; i < NDS_BLOCK_UNITS; ++i) words[i] = snx_ld_le16(at + i * 2);
        e->fn = snx_arm_exec_compile_ex(r->exec, words, NDS_BLOCK_UNITS, pc, 1, &codegen, &consumed);
    } else {
        uint32_t words[NDS_BLOCK_UNITS];
        for (unsigned i = 0; i < NDS_BLOCK_UNITS; ++i) words[i] = snx_ld_le32(at + i * 4);
        e->fn = snx_arm_exec_compile_ex(r->exec, words, NDS_BLOCK_UNITS, pc, 0, &codegen, &consumed);
    }
    if (!e->fn || !consumed) {
        snprintf(error, error_size, "the ARM recompiler rejected the block at 0x%08X", pc);
        return false;
    }
    e->units = (uint8_t)consumed;
    e->bytes = (uint16_t)(consumed * (thumb ? 2 : 4));
    unsigned char* copy = malloc(e->bytes);
    if (!copy) {
        snprintf(error, error_size, "out of memory");
        return false;
    }
    memcpy(copy, at, e->bytes);
    e->code = copy;
    e->source = 2;
    ++r->compiled;
    /* the image's own code, which the offline pass did not find: a hint for the next compile */
    for (uint32_t i = 0; i < r->rom->region_count; ++i) {
        const NdsRegion* region = &r->rom->regions[i];
        if (pc < region->address || (uint64_t)(pc - region->address) + e->bytes > region->size) continue;
        if (memcmp(copy, region->data + (pc - region->address), e->bytes)) continue;
        e->source = 1, e->region = i;
        ++r->hinted;
        break;
    }
    return true;
}

SnxArmBlock nds_recomp_block(NdsRecomp* r, NdsBoard* board, uint32_t pc, int thumb, unsigned* instructions,
                             char* error, size_t error_size) {
    uint32_t key = pc | (uint32_t)thumb;
    const unsigned char* at = board->mem.base + pc;
    uint32_t* fast = &r->fast[(pc >> 1 ^ pc >> 17) & (FAST_SLOTS - 1)];
    const Entry* e = &r->entries[*fast];
    if (e->key == key && *fast && !memcmp(at, e->code, e->bytes)) {
        *instructions = e->units;
        return e->fn;
    }
    if (pc >= 0x0A000000u) {
        snprintf(error, error_size, "the game jumped to 0x%08X, where the console has no memory", pc);
        return NULL;
    }
    if (pc & (thumb ? 1u : 3u)) {
        snprintf(error, error_size, "the game jumped to 0x%08X in %s state", pc, thumb ? "Thumb" : "ARM");
        return NULL;
    }
    uint32_t* slot = slot_for(r, key);
    for (uint32_t i = *slot; i; i = r->entries[i].next) {
        e = &r->entries[i];
        if (!memcmp(at, e->code, e->bytes)) {
            *fast = i;
            *instructions = e->units;
            return e->fn;
        }
    }
    if (!grow(r)) {
        snprintf(error, error_size, "out of memory");
        return NULL;
    }
    slot = slot_for(r, key);
    Entry made = {.key = key, .next = *slot};
    if (from_module(r, at, pc, thumb, &made))
        ++r->from_module;
    else if (!compile(r, at, pc, thumb, &made, error, error_size))
        return NULL;
    r->entries[++r->count] = made;
    *slot = r->count;
    *fast = r->count;
    *instructions = made.units;
    return made.fn;
}

size_t nds_recomp_save_hints(NdsRecomp* r, const char* path) {
    /* the ones the file has already */
    size_t had = 0, capacity = 0, added = 0;
    uint32_t* old = NULL;
    unsigned region, pc;
    FILE* f = fopen(path, "r");
    while (f && fscanf(f, "%x %x", &region, &pc) == 2) {
        if (had == capacity) {
            uint32_t* grown = realloc(old, (capacity = capacity ? capacity * 2 : 1024) * 2 * sizeof *grown);
            if (!grown) break;
            old = grown;
        }
        old[had * 2] = region, old[had * 2 + 1] = pc;
        ++had;
    }
    if (f) fclose(f);
    f = NULL;
    for (uint32_t i = 1; i <= r->count; ++i) {
        const Entry* e = &r->entries[i];
        if (e->source != 1) continue;
        bool known = false;
        for (size_t j = 0; j < had && !known; ++j) known = old[j * 2] == e->region && old[j * 2 + 1] == e->key;
        if (known) continue;
        if (!f && !(f = fopen(path, "a"))) break;
        fprintf(f, "%x %08x\n", e->region, e->key);
        ++added;
    }
    if (f) fclose(f);
    free(old);
    return added;
}
