/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* the services sm hands out, registered under their names, and the events the service files give the game */
#include "nx/hle/services/services_int.h"
#include "nx/hle/services/network/network.h"
#include "hle/services/sm.swipc.h"
#include "hle/services/am.swipc.h"
#include "hle/services/set.swipc.h"
#include "hle/services/time.swipc.h"
#include "hle/services/acc.swipc.h"
#include "hle/services/audio.swipc.h"
#include "hle/services/misc.swipc.h"
#include "hle/services/network/bsd.swipc.h"
#include "hle/services/network/ldn.swipc.h"
#include "hle/services/network/dns.swipc.h"
#include "hle/services/network/nifm.swipc.h"
#include "hle/services/network/ssl.swipc.h"

/* ============================================================ events */
/* a handle to e for a reply: the handle takes its own reference */
u32 ev_handle(u32 ev) {
    KEvent* e = kev(ev);
    return e ? h_new(&e->o) : 0; /* no handle: the game's wait on 0 is refused as an invalid handle */
}

int new_event(u32* event, const char* name, int signaled) {
    KEvent* e = kevent_new(name);
    e->signaled = signaled;
    /* the reference held here keeps the event alive while the handle is made, and goes with it: the game's handle
       is the only one left, or none when the table is full */
    kobj_ref(&e->o);
    *event = h_new(&e->o);
    kobj_unref(&e->o);
    R_SUCCEED();
}

u32 owned_event(const char* name) {
    KEvent* e = kevent_new(name);
    kobj_ref(&e->o);
    return kobj_id(&e->o);
}

/* ============================================================ registration */
void svc_core_exit(void) {
    ldn_exit(); /* its goodbyes go through the room, so before it is left */
    network_room_leave();
}
void svc_core_init(void) {
    snx_users_init(); /* before am: its launch parameter names the user */
    am_init();
    audio_init();
    time_init();
    network_init();
    network_room_init(); /* after the users: the room knows this console by the nickname of the one playing */

    nn_sm_detail_IUserInterface_register(new_sm);
    nn_am_service_IApplicationProxyService_register(new_appletoe);
    nn_am_service_IAllSystemAppletProxiesService_register(new_appletae);
    nn_settings_ISettingsServer_register(new_set);
    nn_settings_ISystemSettingsServer_register(new_setsys);
    nn_timesrv_detail_service_IStaticService_register(new_time);
    svc_register("time:u", new_time_user);
    nn_account_IAccountServiceForApplication_register(new_acc);
    nn_account_IAccountServiceForSystemService_register(new_acc_system);
    /* one interface, two kinds of session (ldr.c) */
    svc_register("ldr:ro", new_ldr);
    svc_register("ro:1", new_ro1);
    nn_audio_detail_IAudioRendererManager_register(new_audren);
    nn_audio_detail_IAudioOutManager_register(new_audout);
    nn_codec_detail_IHardwareOpusDecoderManager_register(new_hwopus);

    /* bsd:u and bsd:a are the application's and the applets' sessions, bsd:s the system's (bsd.c) */
    svc_register("bsd:u", new_bsd_u);
    svc_register("bsd:a", new_bsd_u);
    svc_register("bsd:s", new_bsd_s);
    nn_bsdsocket_cfg_ServerInterface_register(new_bsdcfg);
    nn_socket_resolver_IResolver_register(new_sfdnsres);
    nn_nsd_detail_IManager_register(new_nsd);
    nn_nifm_detail_IStaticService_register(new_nifm);
    nn_ssl_sf_ISslService_register(new_ssl);

    nn_apm_IManager_register(new_apm);
    nn_lm_ILogService_register(new_lm);
    nn_pctl_detail_ipc_IParentalControlServiceFactory_register(new_pctl);
    nn_friends_detail_ipc_IServiceCreator_register(new_friend);
    nn_ns_detail_IServiceGetterInterface_register(new_ns);
    nn_prepo_detail_ipc_IPrepoService_register(new_prepo);
    nn_capsrv_sf_IAlbumApplicationService_register(new_caps);
    nn_err_context_IWriterForApplication_register(new_ectx);
    nn_psm_IPsmServer_register(new_psm);
    nn_fatalsrv_IService_register(new_fatal);
    nn_spl_detail_IRandomInterface_register(new_csrng);
    nn_spl_detail_IGeneralInterface_register(new_spl);
    nn_mmnv_IRequest_register(new_mm);
    nn_nfp_detail_IUserManager_register(new_nfp);
    nn_nfc_detail_IUserManager_register(new_nfc);
    nn_bcat_detail_ipc_IServiceCreator_register(new_bcat);
    nn_aocsrv_detail_IAddOnContentManager_register(new_aoc);
    nn_irsensor_IIrSensorServer_register(new_irs);
    nn_mii_detail_IStaticService_register(new_mii);
    nn_ldn_detail_IUserServiceCreator_register(new_ldn);
    nn_btm_IBtmUser_register(new_btmu);
    nn_audio_detail_IAudioInManager_register(new_audin);
    nn_ngc_detail_IService_register(new_ngc);
    nn_ngct_detail_IService_register(new_ngct);

    /* the audio renderer's 5 ms frames and the audio outs' buffers (audio.c), from the start as on the console */
    if (plat_thread(audio_clock, NULL, (size_t)8 << 20)) fatal("Could not start the audio clock's thread");
}
