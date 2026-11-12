/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* nn::ro, native (ro.hook): what the SDK asks of the program's modules, answered from the runtime's linker
   (runtime/linker.c) in the place of the lists a game's own linker keeps. Installed only when the runtime linked the
   game. Written from what the functions are seen to do with their arguments, never from the SDK's code.
   Answered: the initializers (no ldr:ro session is needed for what follows), an NRO's buffer size, a symbol's address
   by its name, among every module or among the ones after the caller's, and the RELRO passes, which the runtime's
   mapping leaves writable. A function that loads or unloads an NRO, or looks into one, is not answered yet: it
   stops the game by its name. What describes a module by its address (its dynamic segment, name and id) is the
   SDK's own code, which reads the module's header where there is no list of an rtld's */
#include "nx/hle/hook/hook.h"
#include "hle/hook/hooks.hook.h"
#include "nx/runtime/linker.h"
#include "nx/runtime/platform/log.h"
#include "common/utilities/endian.h"

/* nn::ro::ResultNotFound: module 22, description 5 */
#define RO_NOT_FOUND 0xA16u
/* the bytes of BSS an NRO's header gives, at 0x38 of it */
#define RO_NRO_BSS   0x38

static void unanswered(const char* function) {
    fatal("The game called nn::ro::%s, which the runtime's linker does not answer yet. Set hooks_off=" LINKER_FAMILY
          " in the game's settings to run the game's own linker",
          function);
}

void nnro_initialize(CPU* c) { (void)c; }
void nnro_initialize_for_runtime(CPU* c) { (void)c; }
void nnro_initialize_with_port_name(CPU* c, u64 port) { (void)c, (void)port; }
void nnro_finalize(CPU* c) { (void)c; }
/* the buffer an NRO needs beside its image: its BSS */
u32 nnro_get_buffer_size(CPU* c, u64 size, u64 nro) {
    snx_st_le64(hook_host(c, size), snx_ld_le32(hook_host(c, nro + RO_NRO_BSS)));
    return 0;
}
/* the address the program's modules bind name to, as the game's linker would: the first that defines it */
u32 nnro_lookup_symbol(CPU* c, u64 address, u64 name) {
    u64 found = 0;
    if (!linker_find((const char*)hook_host(c, name), &found)) return RO_NOT_FOUND;
    snx_st_le64(hook_host(c, address), found);
    return 0;
}
/* the same among the modules loaded after the caller's, which the return address names */
u32 nnro_lookup_next_symbol(CPU* c, u64 address, u64 name) {
    u64 found = 0;
    if (!linker_find_after(c->x[30], (const char*)hook_host(c, name), &found)) return RO_NOT_FOUND;
    snx_st_le64(hook_host(c, address), found);
    return 0;
}
/* the modules' relocated read-only data stays writable: nothing of the game writes it, and the runtime does not rely
   on the fault */
void nnro_protect_full_relro_for_all_modules(CPU* c) { (void)c; }

u32 nnro_lookup_module_symbol(CPU* c, u64 address, u64 module, u64 name) {
    (void)c, (void)address, (void)module, (void)name;
    unanswered("LookupModuleSymbol");
}
u32 nnro_register_module_info(CPU* c, u64 info, u64 nrr) {
    (void)c, (void)info, (void)nrr;
    unanswered("RegisterModuleInfo");
}
u32 nnro_register_module_info_flag(CPU* c, u64 info, u64 nrr, u32 flag) {
    (void)c, (void)info, (void)nrr, (void)flag;
    unanswered("RegisterModuleInfo");
}
void nnro_unregister_module_info(CPU* c, u64 info) {
    (void)c, (void)info;
    unanswered("UnregisterModuleInfo");
}
u32 nnro_load_module(CPU* c, u64 module, u64 image, u64 buffer, u64 size, i32 flag) {
    (void)c, (void)module, (void)image, (void)buffer, (void)size, (void)flag;
    unanswered("LoadModule");
}
u32 nnro_load_module_check(CPU* c, u64 module, u64 image, u64 buffer, u64 size, i32 flag, int check) {
    (void)c, (void)module, (void)image, (void)buffer, (void)size, (void)flag, (void)check;
    unanswered("LoadModule");
}
void nnro_unload_module(CPU* c, u64 module) {
    (void)c, (void)module;
    unanswered("UnloadModule");
}
/* a module's own RELRO pass, which its start-up code asks for with the module's object of its BSS (all zero here: no
   rtld filled it): left writable too */
void nnro_protect_relro(CPU* c, u64 first, u64 second, u64 third, u64 fourth, u64 fifth) {
    (void)c, (void)first, (void)second, (void)third, (void)fourth, (void)fifth;
}
