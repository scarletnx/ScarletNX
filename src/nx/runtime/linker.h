/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The runtime's linker (nx/loader/relocate.h) and the game's own. A game links itself: the rtld module it carries
   relocates every module and binds their symbols, then enters the SDK (nn::init::Start), which calls the rtld back
   for the modules' constructors and, through nn::ro, for its lists of modules.

   Where it can, the runtime links the game in the rtld's place: it relocates the modules when they are loaded, every
   PLT slot bound, the main thread enters nn::init::Start with three functions of the runtime's (the SDK's exception
   handler is ready; each module's DT_INIT, the last module first; each DT_FINI), and nn::ro is answered by the
   runtime (nn/ro.hook), so that nothing of the SDK reads what an rtld would have left. No code of the rtld runs.

   Where it cannot (NCE, a 32-bit game, a game with no nn::init::Start it knows or whose module does not route
   nn::ro, the nn_ro hooks off, a game that maps NROs), the game's rtld links it as before, which the log says with
   the reason. The runtime then links a copy of the modules' data and, at the game's first call of nn::init::Start,
   compares every relocated word of the game's memory with the copy: the log says how many, or the first that
   differs */
#ifndef SNX_RUNTIME_LINKER_H
#define SNX_RUNTIME_LINKER_H
#include "common/cpu/aarch64/cpu.h"

/* the hook family of nn::ro (nn/ro.hook): installed only when the runtime links the game */
#define LINKER_FAMILY "nn_ro"

/* once the modules are loaded and their symbols read, before the hooks: decides who links, and links */
void linker_decide(void);
/* 1 when the runtime linked the game */
int linker_links(void);
/* once the hooks have taken their dispatch slots, before the game's first thread */
void linker_init(void);
/* the main thread's entry, given the game's own (its rtld's): nn::init::Start when the runtime linked the game */
u64 linker_entry(u64 entry);
/* nn::init::Start's arguments after the thread's handle (x1 to x4: the address behind the rtld, where the game's
   arguments would lie, and the runtime's three callbacks), into args: 1, or 0 when the game's linker runs */
int linker_main(u64 args[4]);
/* the address the game's modules bind name to: 1 with it in *address, 0 for a name nothing defines or when the
   game's linker runs */
int linker_find(const char* name, u64* address);

#endif
