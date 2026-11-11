/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The NROs a program maps while it runs (ldr:ro), as a game module tells the runtime of them: what snx_nros() of a
   module returns. The recompiler finds them in the program's RomFS and gives each the guest address it makes its code
   for. slots[0..count) are in the order of their hash, the SHA-256 of the NRO's image as ldr:ro checks it against an
   NRR, and one after another in memory from lo, behind the program's own modules, to hi: CPU.disp covers
   [code_lo, hi) when the module has the table. An NRO's code is a module of its own, <hash>.so in the folder
   <Game>_nro beside the game module, whose SnxModule names the NRO as its one module at that base. A separate symbol
   with a version of its own, as snx_routes (module_abi.h): a module without NROs has none, and SnxModule and SNX_ABI
   stay as they are. Plain C99 with no other header's types: the recompiler's loader and driver read it too */
#ifndef SNX_NX_NRO_SLOTS_H
#define SNX_NX_NRO_SLOTS_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define SNX_NROS_VERSION 1
#define SNX_NROS_FOLDER  "_nro"
typedef struct SnxNroSlot {
    uint8_t hash[32];
    uint64_t base, size; /* size: the image and its BSS, in whole pages */
} SnxNroSlot;
typedef struct SnxNros {
    uint32_t version;
    uint32_t count;
    uint64_t lo, hi;
    const SnxNroSlot* slots;
} SnxNros;

/* What snx_imports() of an NRO's module returns: the functions of the game's modules its code calls directly, by
   their guest addresses in ascending order, and the pointer it calls each through. A pointer starts as dynamic, the
   module's own function that looks the address up in CPU.disp; the runtime puts the game module's function there
   when it has one for the address and its dispatch slot holds that function (no hook took it), and the module's
   lookup again when the slot is given another function later, while guest threads run: the module reads a pointer
   with a relaxed atomic load. The pointers are functions of the guest's signature, void(CPU*, u64) */
#define SNX_NRO_IMPORTS_VERSION 1
typedef struct SnxNroImports {
    uint32_t version;
    uint32_t count;
    const uint64_t* address;
    void** function;
    void* dynamic;
} SnxNroImports;

/* the place of the NRO of hash in nros (NULL: no table), or -1 when it has none */
static inline long snx_nro_slot(const SnxNros* nros, const uint8_t* hash) {
    size_t lo = 0, hi = nros ? nros->count : 0;
    while (lo < hi) {
        size_t middle = lo + (hi - lo) / 2;
        int order = memcmp(nros->slots[middle].hash, hash, 32);
        if (!order) return (long)middle;
        if (order < 0)
            lo = middle + 1;
        else
            hi = middle;
    }
    return -1;
}

#endif
