/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* nn::ro, native (ro.hook): what the SDK asks of the program's modules, answered from the runtime's linker
   (runtime/linker.c) in the place of the lists a game's own linker keeps. Installed only when the runtime linked the
   game. Written from what the functions are seen to do with their arguments, never from the SDK's code.
   Answered: the initializers (no ldr:ro session is needed for what follows), an NRO's buffer size, a symbol's address
   by its name, among every module or among the ones after the caller's, the RELRO passes, which the runtime's
   mapping leaves writable, and the NROs: a list of their hashes (an NRR) registered and unregistered, an NRO loaded,
   linked and unloaded, and a symbol of one by its name. What describes a module by its address (its dynamic segment,
   name and id) is the SDK's own code, which reads the module's header where there is no list of an rtld's.
   The game's objects are filled as the SDK is seen to leave them: a Module's first word names the module's own object
   in its BSS, which stays zero here, then its state (1 loaded, 0 not), the image and the buffer it was loaded from; a
   RegistrationInfo is its state (1 registered), the NRR and a list's node, which links to itself */
#include "nx/hle/hook/hook.h"
#include "hle/hook/hooks.hook.h"
#include "nx/hle/services/services.h"
#include "nx/runtime/linker.h"
#include "nx/runtime/main.h"
#include "nx/runtime/platform/log.h"
#include "common/utilities/endian.h"
#include <string.h>

/* nn::ro's results, module 22: an image that is no NRO (4), a name nothing defines (5), an image that is no NRR (6) */
#define RO_NOT_NRO   0x816u
#define RO_NOT_FOUND 0xA16u
#define RO_NOT_NRR   0xC16u
/* an NRO's header: its magic, its size and the bytes of BSS it needs */
#define RO_NRO_MAGIC 0x10
#define RO_NRO_SIZE  0x18
#define RO_NRO_BSS   0x38
/* an NRR's size in its header, 0x180 bytes further on in an Ounce one (hle/services/ldr.c) */
#define RO_NRR_SIZE  0x338
/* a Module and a RegistrationInfo of the game's, as the SDK leaves them */
enum { MODULE_OBJECT = 0, MODULE_STATE = 8, MODULE_IMAGE = 0x10, MODULE_BUFFER = 0x18 };
enum { INFO_STATE = 0, INFO_NRR = 8, INFO_NODE = 0x10 };

/* ldr:ro's result as the SDK takes it: nn::ro's own (module 22, descriptions 1 to 1023) go to the game, and on any
   other failure the SDK stops the game */
static u32 result(const char* function, int r) {
    if (!r || ((r & 0x1FF) == 22 && (((u32)r >> 9) & 0x1FFF) - 1 < 0x3FF)) return (u32)r;
    fatal("nn::ro::%s: loading the game's modules failed with the result %#x, which the SDK stops the game on",
          function, (unsigned)r);
}

void nnro_initialize(CPU* c) { (void)c; }
void nnro_initialize_for_runtime(CPU* c) { (void)c; }
/* the port names the kind of NRR a registration with a process handle asks for: ro:1's, or ldr:ro's */
void nnro_initialize_with_port_name(CPU* c, u64 port) {
    ldr_own_kind(!strcmp((const char*)hook_host(c, port), "ro:1"));
}
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

/* the same in one loaded NRO, by its own definition */
u32 nnro_lookup_module_symbol(CPU* c, u64 address, u64 module, u64 name) {
    u64 found = 0;
    if (!linker_nro_find(snx_ld_le64(hook_host(c, module + MODULE_OBJECT)), (const char*)hook_host(c, name), &found))
        return RO_NOT_FOUND;
    snx_st_le64(hook_host(c, address), found);
    return 0;
}
/* an NRR registered with ldr:ro's code: process NULL, or the handle the game gives for its process */
static u32 register_info(CPU* c, u64 info, u64 nrr, const u32* process) {
    u32 r;
    if (memcmp(hook_host(c, nrr), "NRR0", 4)) return RO_NOT_NRR;
    r = result("RegisterModuleInfo",
               ldr_own_register(nrr, snx_ld_le32(hook_host(c, nrr + RO_NRR_SIZE + (g_ounce ? 0x180 : 0))), process));
    if (r) return r;
    snx_st_le32(hook_host(c, info + INFO_STATE), 1);
    snx_st_le64(hook_host(c, info + INFO_NRR), nrr);
    snx_st_le64(hook_host(c, info + INFO_NODE), info + INFO_NODE);
    snx_st_le64(hook_host(c, info + INFO_NODE + 8), info + INFO_NODE);
    return 0;
}
u32 nnro_register_module_info(CPU* c, u64 info, u64 nrr) { return register_info(c, info, nrr, NULL); }
u32 nnro_register_module_info_process(CPU* c, u64 info, u64 nrr, u32 process) {
    return register_info(c, info, nrr, &process);
}
void nnro_unregister_module_info(CPU* c, u64 info) {
    u64 nrr = snx_ld_le64(hook_host(c, info + INFO_NRR));
    int r = ldr_own_unregister(nrr);
    if (r)
        fatal("nn::ro::UnregisterModuleInfo: the list of modules at %llx is not registered (the result %#x), which "
              "the SDK stops the game on",
              (unsigned long long)nrr, (unsigned)r);
    snx_st_le32(hook_host(c, info + INFO_STATE), 0);
}
/* flag asks for each PLT slot bound at its first call or at once: the runtime's linker binds them at once either
   way. check: the game loads the module for no other to bind to */
u32 nnro_load_module_check(CPU* c, u64 module, u64 image, u64 buffer, u64 size, i32 flag, int check) {
    const u8* nro = hook_host(c, image);
    u64 base = 0, mod0;
    u32 r;
    (void)flag;
    if (memcmp(nro + RO_NRO_MAGIC, "NRO0", 4)) return RO_NOT_NRO;
    r = result("LoadModule",
               (int)linker_nro_load(c, image, snx_ld_le32(nro + RO_NRO_SIZE), buffer, size, check, &base));
    if (r) return r;
    mod0 = base + snx_ld_le32(hook_host(c, base + 4));
    snx_st_le64(hook_host(c, module + MODULE_OBJECT), mod0 + snx_ld_le32(hook_host(c, mod0 + 0x18)));
    snx_st_le32(hook_host(c, module + MODULE_STATE), 1);
    snx_st_le64(hook_host(c, module + MODULE_IMAGE), image);
    snx_st_le64(hook_host(c, module + MODULE_BUFFER), buffer);
    return 0;
}
u32 nnro_load_module(CPU* c, u64 module, u64 image, u64 buffer, u64 size, i32 flag) {
    return nnro_load_module_check(c, module, image, buffer, size, flag, 0);
}
void nnro_unload_module(CPU* c, u64 module) {
    u64 object = snx_ld_le64(hook_host(c, module + MODULE_OBJECT));
    if (!linker_nro_unload(c, object))
        fatal("nn::ro::UnloadModule: the game unloads a module (its object at %llx) that is not loaded",
              (unsigned long long)object);
    snx_st_le32(hook_host(c, module + MODULE_STATE), 0);
}
/* a module's own RELRO pass, which its start-up code asks for with the module's object of its BSS (all zero here: no
   rtld filled it): left writable too */
void nnro_protect_relro(CPU* c, u64 first, u64 second, u64 third, u64 fourth, u64 fifth) {
    (void)c, (void)first, (void)second, (void)third, (void)fourth, (void)fifth;
}
