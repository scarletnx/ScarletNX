/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Registering the HLE services, for startup. The services' own declarations are in services_int.h */
#ifndef SNX_SERVICES_H
#define SNX_SERVICES_H
#include "nx/hle/kernel/ipc.h"

/* each registers its family's service names with the service manager */
void svc_core_init(void);
void svc_core_exit(void); /* at exit, what the others hear of this console leaving (the Eden room) */
void svc_fs_init(void);
void svc_hid_init(void);
void svc_vi_init(void);
void svc_pl_init(void);
/* the game's display version from its control.nacp ("1.0.0" without one), terminated, for the log's header */
void am_display_version(char version[16]);
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
/* how many nn::settings::Language values set knows, to check the language the settings file names */
int set_language_count(void);
#include "common/utilities/users.h" /* SNX_USERS_MAX, the .ini's user at most */
#endif
