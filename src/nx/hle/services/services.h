/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Registering the HLE services, for startup. The services' own declarations are in services_int.h */
#ifndef SNX_SERVICES_H
#define SNX_SERVICES_H
#include "nx/runtime/state_fns.h"
#include "nx/hle/kernel/ipc.h"
#include "common/utilities/game_ini.h" /* SnxNext, ExecuteProgram's hand-over */

/* each registers its family's service names with the service manager */
void svc_core_init(void);
void svc_core_exit(void); /* at exit, what the others hear of this console leaving (the Eden room) */
void svc_fs_init(void);
void svc_hid_init(void);
void svc_vi_init(void);
void svc_pl_init(void);
/* the game's display version from its control.nacp ("1.0.0" without one), terminated, for the log's header */
void am_display_version(char version[16]);
/* the same, the control.nacp's alone: 0 with version "" when it has none (no "1.0.0" in its place) */
int am_display_version_of(char version[16]);
/* the sizes of the game's save data of a type (nn::fs::SaveDataType: 1 an account's, 3 the device's), as the system made
   it: what ExtendSaveData gave it, else the NACP's, 64 MiB each without a NACP. 0 and 0 for another type with a NACP */
void am_save_data_size(u8 type, i64* normal_size, i64* journal_size);
/* whether the game's control.nacp sets user_account_switch_lock, which acc answers for the SDK (0 without one) */
int am_user_account_switch_lock(void);
/* the ID the running game's add-on content is numbered from: AddOnContentBaseId in its control.nacp, which a game
   whose DLCs are numbered otherwise than its own ID sets, else the SDK's default for its ID (snx_nacp_aoc_base) */
u64 am_aoc_base_id(void);
/* a game pause: the audio clock's thread stops, audio_hold returning once it has, until audio_release (audio.c) */
void audio_hold(void);
void audio_release(void);
/* save states (runtime/state.c): the host's side of the audio outs and the Opus decoders (their streams, FFmpeg's
   decoders) goes before a load writes g_hle over it, and is made again for what the loaded state holds */
void audio_state_drop(void);
void audren_state_drop(void); /* the renderers' states and host outputs, which a load makes again from the state's */
void audio_state_rebuild(void);
/* The software keyboard's box (am.c): its answer, from the main thread once it holds no UI lock, as am shows the box
   under its own: the text given, or cancelled, for the box of ticket (an answer to another is said and dropped). And
   after a save state's load, the game held: the box shown before goes, and an inline keyboard the state shows gets
   one again */
void am_keyboard_answered(u32 ticket, int cancelled, const char* text);
void am_state_loaded(void);
/* the TV mode changed while the game runs: am's display resolution change event, and the performance mode message */
void am_display_mode_changed(void);
/* A collection's program starting another (nn::oe::ExecuteProgram, am's 120), as game_ini.h's SnxNext hands it over.
   am_handover_out: the program asked for, its index (-1 for none, then nothing else is filled), with the user channel's
   storages copied into next (owned there), this program's index as the previous one and the open user acc stored.
   am_handover_in: this program's index (main.c's g_program), and what the one that started it handed over, NULL for a
   program the system started */
int am_handover_out(SnxNext* next);
void am_handover_in(int program, const SnxNext* next);
/* ldr:ro for the runtime's linker, where it answers nn::ro in the SDK's place (nn/ro.c) and the SDK opens no session:
   what the session's commands of those names do, on a state of the runtime's own, each the command's result.
   ldr_own_kind: the port the SDK was told to use, ldr:ro (0) or ro:1 (1), whose kind of NRR a registration with a
   process handle asks for; process NULL: RegisterModuleInfo, else RegisterProcessModuleInfo with that handle */
void ldr_own_kind(int kind);
int ldr_own_register(u64 nrr, u64 size, const u32* process);
int ldr_own_unregister(u64 nrr);
int ldr_own_load(u64 nro, u64 size, u64 bss, u64 bss_size, u64* address);
int ldr_own_unload(u64 address);
/* the functions a save state may hold of each file (nx/runtime/state_fns.h) */
extern const StateFnTable g_acc_state_fns;    /* acc.c */
extern const StateFnTable g_am_state_fns;     /* am.c */
extern const StateFnTable g_audio_state_fns;  /* audio.c */
extern const StateFnTable g_audren_state_fns; /* audren.c */
extern const StateFnTable g_ldr_state_fns;    /* ldr.c */
extern const StateFnTable g_misc_state_fns;   /* misc.c */
/* how many nn::settings::Language values set knows, to check the language the settings file names */
int set_language_count(void);
#include "common/utilities/users.h" /* SNX_USERS_MAX, the .ini's user at most */
#endif
