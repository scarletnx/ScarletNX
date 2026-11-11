/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_CTR_MODULE_H
#define SNX_CTR_MODULE_H
#include "3ds/runtime/ctr_process.h"
#define SNX_CTR_MODULE_MAGIC         0x31525443584e5353ull
#define SNX_CTR_MODULE_ABI           3u /* 2: checked VFP transfers of up to 128 bytes; 3: hosted blocks */
#define SNX_CTR_MODULE_CODEGEN_ABI   1u
#define SNX_CTR_MODULE_CHECKED_MODE  1u
#define SNX_CTR_MODULE_HOSTED_MODE   2u
/* the memory model a module of this host is compiled for: the hosted flat space where the host has one
   (memory/host.h, SNX_ARM_MEMORY_HOSTED), the checked bus elsewhere */
unsigned snx_ctr_module_memory(void);
/* Offline compilation bounds, NOT a general title-support guarantee. Tiny
   images retain exhaustive entries; larger images use conservative static CFG
   discovery. Uncovered indirect targets remain explicit unsupported exits. */
#define SNX_CTR_AOT_TEXT_LIMIT       (16u * 1024u * 1024u)
#define SNX_CTR_AOT_EXHAUSTIVE_LIMIT 4096u

typedef struct {
    uint64_t magic;
    uint32_t abi, descriptor_size, cpu_size, checked_size;
    uint32_t arm_abi, checked_abi, codegen_abi, profile, memory_mode, pointer_size;
    uint32_t text_address, text_bytes, arm_count, thumb_count;
    unsigned char image_identity[32];
    SnxArmBlock const* arm_blocks;
    SnxArmBlock const* thumb_blocks;
} SnxCtrModule;
/* ABI checks are compatibility checks, NOT a native-code sandbox. Only load
   freshly generated modules from trusted private host staging directories. */
bool snx_ctr_module_compatible(const SnxCtrModule* module, const SnxCtrImage* image);
/* Whether the descriptor's fields before its block tables are those this build compiles for image */
bool snx_ctr_module_fields(const SnxCtrModule* module, const SnxCtrImage* image);
typedef enum {
    SNX_CTR_RUN_EXIT,
    SNX_CTR_RUN_FAULT,
    SNX_CTR_RUN_UNSUPPORTED,
    SNX_CTR_RUN_BUDGET,
    SNX_CTR_RUN_BLOCKED
} SnxCtrRunResult;
/* Blocks between the dispatcher's scheduler polls: at about 2 million blocks a second, about 2 ms, inside the DSP's
   4.9 ms audio frame and the display's 16.7 ms */
#define SNX_CTR_POLL_BLOCKS 4096u
/* Requires a compatible module and its matching freshly initialized process.
   Bounded static blocks; no discovery, compiler, interpreter or JIT here.
   Process-owned kernel HLE handles a bounded SVC subset. Unknown calls stop.
   A thread that waits gives the core to another; BLOCKED means no thread can
   ever run again (no deadline and nothing left to signal). */
SnxCtrRunResult snx_ctr_dispatch(const SnxCtrModule* module, SnxCtrProcess* process, uint64_t limit,
                                 uint64_t* executed);
typedef struct {
    void* library;
    const SnxCtrModule* module;
} SnxCtrNative;
/* Trusted native code only: dlopen may execute constructors before ABI checks.
   Never accept arbitrary host modules as though these checks were a sandbox. */
bool snx_ctr_native_open(const char* path, const SnxCtrImage* image, SnxCtrNative* native, char* error,
                         size_t error_size);
void snx_ctr_native_close(SnxCtrNative* native);
/* Whether the module at path was compiled for image by this build, read from its descriptor in the file without
   loading it: the run compiles first when it was not, so it loads a module once (a library unloaded and loaded again
   by its path can be the old one, as under the Tracy client, whose dlclose does nothing) */
bool snx_ctr_module_current(const char* path, const SnxCtrImage* image);
/* A game's folder is laid out as a Switch game's is: recompiled/<title ID>/ holds <title ID>_code.so (_code.dll on
   Windows), the module, <title ID>.hints, the blocks runs compiled at run time, <title ID>.ini, what a copy of the
   runtime named <title ID> beside them plays, and the copy itself. The file of the folder's own name with suffix, the
   caller's to free; NULL out of memory */
#ifdef _WIN32
#define SNX_CTR_MODULE_SUFFIX "_code.dll"
#else
#define SNX_CTR_MODULE_SUFFIX "_code.so"
#endif
char* snx_ctr_game_file(const char* directory, const char* suffix);
/* The title's module (snx_ctr_game_file's SNX_CTR_MODULE_SUFFIX) is compiled when it is missing, when it doesn't match the
   image (an older build, a changed .code), or when SNX_CTR_RECOMPILE asks, then run unless SNX_CTR_COMPILE_ONLY: until
   the game exits or its window closes, for limit blocks when limit is not 0, or for seconds when it is above 0. The title is the extraction in
   directory, or with game the game read where it lies, directory then its state folder. The module is
   trusted as code the driver compiled itself, never as a sandbox. */
enum { SNX_CTR_RECOMPILE = 1, SNX_CTR_COMPILE_ONLY = 2 };
/* ipc_stubs: the services whose unknown commands answer success instead of stopping when the ctr_auto_stub_ipc setting
   is off (snx_ctr_ipc_stub_mask). runtime: the program copied into the folder as the game's own once its module is
   ready (snx_ctr_game_file's name, .exe on Windows), NULL for none */
int snx_ctr_aot_run(const char* directory, const SnxCtrSource* game, SnxCtrModel model, uint64_t limit, double seconds,
                    unsigned flags, uint32_t ipc_stubs, const char* runtime, char* error, size_t error_size);
#endif
