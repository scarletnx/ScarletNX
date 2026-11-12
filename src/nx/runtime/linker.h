/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The runtime's linker (nx/loader/relocate.h) beside the game's own. A game links itself: the rtld module it carries
   relocates every module and binds their symbols before it enters the SDK. The runtime links a copy of the modules'
   data when they are loaded, and once the game's linker is done, as it calls nn::init::Start, compares every
   relocated word of the game's memory with the copy and says in the log how many there are and the first that
   differs. The game runs as before: its memory is only read */
#ifndef SNX_RUNTIME_LINKER_H
#define SNX_RUNTIME_LINKER_H

/* once the modules are loaded and the hooks have taken their dispatch slots, before the game's first thread */
void linker_init(void);

#endif
