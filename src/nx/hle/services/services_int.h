/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What the service files share: the event helpers, each family's constructors and init, and the includes every
   service file relies on */
#ifndef SNX_SERVICES_INT_H
#define SNX_SERVICES_INT_H
#include "nx/hle/services/services.h"
#include "nx/hle/kernel/ipc.h"
#include "nx/hle/kernel/kernel.h"
#include "nx/hle/services/audren_dsp.h"
#include "nx/memory/mem.h"
#include "nx/runtime/main.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/overlay.h" /* the keyboard's box's line, which am's answer holds */
#include "nx/runtime/platform/plat.h"
#include <stdlib.h>
#include <string.h>

/* ---- services.c */
/* The services keep their events and shared memories as kernel object ids (kobj_id), which a save state holds as they
   are: kev and kshm are the objects again, and ev_signal, ev_clear and shm_ptr act on them by id */
/* An id that names no object of the type (0, or one a damaged save state held) gives NULL, and ev_signal and ev_clear
   do nothing with it. The shared memories' ids are checked when a state is loaded */
static inline KEvent* kev(u32 id) {
    KObj* o = kobj_at(id);
    return o && o->type == KO_EVENT ? (KEvent*)o : NULL;
}
static inline KShmem* kshm(u32 id) {
    KObj* o = kobj_at(id);
    return o && o->type == KO_SHMEM ? (KShmem*)o : NULL;
}
static inline void ev_signal(u32 id) {
    KEvent* e = kev(id);
    if (e) kevent_signal(e);
}
static inline void ev_clear(u32 id) {
    KEvent* e = kev(id);
    if (e) kevent_clear(e);
}
static inline u8* shm_ptr(u32 id) { return kshmem_ptr(kshm(id)); }
u32 ev_handle(u32 ev); /* a handle to the event for a reply, with its own reference */
/* a new event for a reply that only the game holds, signaled or not. Returns 0, for a handler to return */
int new_event(u32* event, const char* name, int signaled);
u32 owned_event(const char* name); /* a new event the service keeps a reference on: its id */

/* ---- sm.c */
SvcObj* new_sm(void);

/* ---- am.c */
extern const u64 g_user_id[2];
void am_init(void); /* the applet events, and the messages queued at startup */
SvcObj* new_appletoe(void);
SvcObj* new_appletae(void);

/* ---- users.c: the console's users (common/utilities/users.h) */
void snx_users_init(void); /* snx_users_load of the data folder's users/, the game stopped if it says no */

/* ---- set.c */
void set_language_code(int lang, u8 code[8]); /* the 8-byte LanguageCode, en-US for an index out of range */
SvcObj* new_set(void);
SvcObj* new_setsys(void);

/* ---- time.c */
void time_init(void);
SvcObj* new_time(void);      /* time:a and time:s */
SvcObj* new_time_user(void); /* time:u, an application's: no write */

/* ---- acc.c */
SvcObj* new_acc(void);

/* ---- ldr.c: ldr:ro and ro:1 */
SvcObj* new_ldr(void);
SvcObj* new_ro1(void);

/* ---- audio.c */
void audio_init(void); /* the renderer's event, before the audio clock runs */
void audio_clock(void* arg);
SvcObj* new_audren(void);
SvcObj* new_audout(void);
SvcObj* new_hwopus(void);

/* ---- misc.c: the small services */
SvcObj* new_apm(void);
SvcObj* new_lm(void);
SvcObj* new_pctl(void);
SvcObj* new_friend(void);
SvcObj* new_ns(void);
SvcObj* new_prepo(void);
SvcObj* new_psm(void);
SvcObj* new_fatal(void);
SvcObj* new_csrng(void);
SvcObj* new_mm(void);
SvcObj* new_nfp(void);
SvcObj* new_nfc(void);
SvcObj* new_caps(void);
SvcObj* new_bcat(void);
SvcObj* new_aoc(void);
SvcObj* new_irs(void);
SvcObj* new_mii(void);
SvcObj* new_ectx(void);
SvcObj* new_spl(void);
SvcObj* new_btmu(void);
SvcObj* new_audin(void);
u32 apm_configuration(u32 mode); /* apm's performance configuration of a mode, Normal (0) or Boost */
void apm_set_configuration(u32 mode, u32 config);
void apm_cpu_boost(u32 boost); /* am's SetCpuBoostMode: Boost mode's configuration for it */
SvcObj* new_ngc(void);
SvcObj* new_ngct(void);

/* ---- audren.c: the audio renderer */
int audren_update(SvcObj* o, IpcBuf in, IpcBuf out); /* RequestUpdate, on its input and output buffers */
int audren_open(SvcObj* o, const u8* params);        /* 0 when the renderer can't be made */
void audren_tick(void);                              /* one frame of every started renderer */
void audren_volume(float gain); /* the output device's volume (audio.c), on every renderer's host output */

/* ---- HLE state in g_hle (hle.h): each file's, and the pools of its objects. The data of a service object (the
   types starting with u32 obj) comes from svcobj_new_data, under ipc.c's lock, which guards every such pool */

/* ---- acc.c */
#define SNX_ACC_PICTURE_MAX 0x20000 /* the most of the user's picture a game takes */
/* the profiles and account managers a game holds at once: one or two for each user (eight at most on a console),
   with room for a game that keeps one for each it asked */
#define ACC_USERS 256
/* an IProfile's or IManagerForApplication's: the user it was made for */
typedef struct {
    u32 obj;
    u64 uid[2];
} AccUser;
typedef struct {
    /* the picture of a user with none of its own, drawn at the first ask and kept */
    struct {
        u32 size;
        int made, overflow; /* made: drawn or tried; overflow: more than SNX_ACC_PICTURE_MAX came */
        u8 d[SNX_ACC_PICTURE_MAX];
    } pic;
    AccUser users[ACC_USERS];
    u32 user_next;
    /* StoreOpenContext ran: the open user is kept for the program ExecuteProgram starts next */
    int context_stored;
} HleAcc;
/* acc's host side: the picture's lock (hle.h's g_host) */
typedef struct {
    PMutex pic_lk;
} HostAcc;

/* ---- am.c */
#define AM_MESSAGES     16 /* the application's message queue */
#define AM_USER_CHANNEL 8
#define AM_IN_DATA      8  /* the storages a library applet holds: those the game pushed */
#define AM_OUT_DATA     16 /* and its interactive replies */
#define SWKBD_TEXT_CHARS 0x1fa /* the inline keyboard's calc argument's input text, UTF-16 */
/* the storages a game holds at once: launch parameters, the user channel's AM_USER_CHANNEL, each library applet's
   AM_IN_DATA and AM_OUT_DATA, and those the game keeps between them */
#define AM_STORAGES 512
#define AM_ACCESSORS 256 /* a storage's accessors: one each while the game reads or writes it, usually just opened */
#define AM_VOLUMES   64  /* the audio controllers: one per proxy a game opens, which it does a few times */
#define AM_LOCKS     64  /* the lock accessors: one for each button reader or writer a game asks for */
/* the library applets: one runs at a time, and a game keeps few accessors after (a keyboard inline, one shown) */
#define AM_APPLETS 32
/* IStorage's data: its bytes in d, or a transfer memory's guest pages */
typedef struct {
    u32 obj;
    int handle_only; /* CreateHandleStorage's: only OpenTransferStorage opens it */
    u64 size;
    u64 guest; /* where the transfer memory's pages are, 0 for bytes kept here */
    u32 tmem;  /* the transfer memory's id */
    u8* d; /* size bytes (one at least), the storage's own allocation, freed with it */
} StorageData;
/* IStorageAccessor's and ITransferStorageAccessor's: the storage they hold a reference on */
typedef struct {
    u32 obj;
    SvcObj* storage;
} AmAccessor;
/* IAudioController's volumes, from 0 to 1: what the game expects itself and a library applet to play at */
typedef struct {
    u32 obj;
    float main_volume, library_volume;
} AmVolumes;
/* ILockAccessor's */
typedef struct {
    u32 obj;
    int locked;
    u32 event;
} AmLock;
/* ILibraryAppletAccessor's */
typedef struct {
    u32 obj;
    u32 applet_id, mode;
    u32 state_ev, out_ev, interactive_ev;
    SvcObj* in[AM_IN_DATA]; /* the storages the game pushed, in order (common arguments first) */
    int nin;
    SvcObj* out[AM_OUT_DATA]; /* the interactive replies, oldest first */
    int nout;
    int out_popped;
    int completed;
    /* the inline keyboard's state, its text (UTF-16) and the reply encoding the game asked for */
    u32 kb_state, kb_max;
    i32 kb_cursor;
    int kb_utf8;
    u32 kb_len;
    u16 kb_text[SWKBD_TEXT_CHARS];
} LibApplet;
typedef struct {
    /* the application's message queue: a message stays queued until ReceiveMessage takes it, and the event is
       signaled while one is */
    u32 msg_event;
    u32 sleep_lock_event; /* the sleep lock is granted at once, so this is signaled from the first request on */
    u32 launchable_event; /* library applets can be launched from the start */
    u32 resolution_event; /* the default display resolution and HDCP state never change while running */
    SvcObj* user_channel[AM_USER_CHANNEL]; /* copies of the storages unpopped, the last one on top */
    u32 user_count;
    u8 vr_mode; /* as the game turned it: the screen is not split for it */
    u32 msgs[AM_MESSAGES];
    u32 head, count;
    int preselected_popped; /* the preselected user launch parameter is given once */
    u64 next_layer;         /* CreateManagedDisplayLayer's ids, from 1 */
    u32 idle_extension;     /* ISelfController's two settings a game reads back, as it made them */
    u8 auto_sleep_disabled;
    u8 vr_mode_3d;          /* on between BeginVrMode3d and EndVrMode3d */
    u8* nacp;               /* the game's control.nacp in its data folder, read on first use; NULL with none */
    u64 nacp_size;
    int nacp_read;
    /* the normal and journal sizes ExtendSaveData gave the account's (0) and the device's (1) save, which
       GetSaveDataSize answers from then on, for the rest of the run */
    i64 save_size[2][2];
    int save_extended[2];
    /* the objects' data */
    StorageData storages[AM_STORAGES];
    AmAccessor accessors[AM_ACCESSORS];
    AmVolumes volumes[AM_VOLUMES];
    AmLock lock_objs[AM_LOCKS];
    LibApplet applets[AM_APPLETS];
    u32 storage_next, accessor_next, volume_next, lock_obj_next, applet_next;
} HleAm;
/* am's host side: locks and condition variables, which a save state never holds (hle.h's g_host). And under lock, the
   software keyboard's box over the game (am.c), the host's too: the ticket of the box shown last (0 none), whether it
   waits for its answer and who for, the inline keyboard of an applet (its slot + 1 and object) or, kb_inline 0, the
   thread of a whole applet, waiting on kb_cv; and that thread's answer, the ticket answered with its text or a cancel */
typedef struct {
    PMutex lock;
    PCond kb_cv;
    u32 kb_ticket;
    int kb_open;
    u32 kb_inline, kb_obj;
    u32 kb_answered;
    int kb_cancelled;
    char kb_text[OVERLAY_KB_TEXT];
} HostAm;

/* ---- audio.c */
#define AUDOUT_BUFFERS 64 /* appended and released buffers an audio out holds */
#define AUDOUT_MAX     32 /* the audio outs open at once: the console's audout:u has a dozen (Eden's 12 sessions) */
#define AUDDEV_MAX     32 /* the IAudioDevices: one per GetAudioDeviceService, a few in a game's run */
/* the firmware keeps 24 decoders (OutOfOpusDecoders after); a closing one is still in the pool after the count of
   open ones dropped, which a decoder opened on another thread meanwhile may need room beside */
#define SNX_HWOPUS_MAX_DECODERS 24
#define HWOPUS_POOL             (2 * SNX_HWOPUS_MAX_DECODERS)
struct AVCodecContext;
struct AVPacket;
struct AVFrame;
/* an IAudioOut: its buffers in one ring, the released ones first, oldest to newest, then the ones still queued */
typedef struct AudOut {
    u32 obj;
    u32 channels, started;
    float volume;
    struct AudOut* next; /* in g_hle.audio.outs */
    PAudio* output;      /* NULL without a host device: the buffers are still played on the clock */
    u32 ev;              /* the buffer event: a buffer released */
    u64 tag[AUDOUT_BUFFERS];
    u32 frames[AUDOUT_BUFFERS];
    u32 first, nreleased, nqueued;
    u32 head_played; /* frames of the first queued buffer already played */
    u64 t0;          /* the guest time (guest_ns) the clock started counting the queued buffers at */
    u64 t0_frames;   /* frames played since t0 */
    u64 played;      /* GetAudioOutPlayedSampleCount */
} AudOut;
/* an IAudioRenderer. Its play state is the object's u[0] (SNX_AUDREN_STARTED or SNX_AUDREN_STOPPED), which the clock
   reads in audren_tick; audren.c keeps its renderer's slot in u[1] */
typedef struct {
    u32 obj;
    u32 sample_rate, sample_count, mix_buffer_count; /* the parameters it was opened with, read back */
    u32 limit;
    float voice_drop; /* as the game set it, read back: no voice is dropped here */
    u32 manual;       /* OpenAudioRendererForManualExecution: no system event */
} AudRen;
/* an IAudioDevice's system, input and output events */
typedef struct {
    u32 obj;
    u32 ev[3];
} AudDevice;
/* a hwopus decoder (audio.c) */
typedef struct {
    u32 obj;
    u32 rate, channels, step;
    /* how it was opened, for a save state's load to open it again: the streams and their channels' places, mapped
       set for a multistream decoder */
    u32 streams, stereo_streams, mapped;
    u8 mapping[255];
    PMutex lk;
#ifndef SNX_NO_H264
    struct AVCodecContext* dec;
    struct AVPacket* pkt;
    struct AVFrame* frame;
#endif
    i16* queue;  /* channels * cap samples, interleaved, at the decoder's rate */
    float* taps; /* the filter's ntaps coefficients (none at 48 kHz) */
    float* hist; /* the last ntaps samples of each channel at 48 kHz, a ring at hpos */
    u32 queued, cap, phase, ntaps, hpos;
} HwOpus;
typedef struct {
    u32 ev;              /* the renderers' system event, every frame */
    AudOut* outs;        /* every open audio out, for the clock */
    float device_volume; /* the output device's, as SetAudioDeviceOutputVolume set it */
    u32 hwopus_live;     /* decoders open, across every hwopus session */
    AudOut outs_pool[AUDOUT_MAX];
    AudRen rens[SNX_AUDREN_MAX];
    AudDevice devices[AUDDEV_MAX];
    HwOpus opus[HWOPUS_POOL];
    u32 out_next, ren_next, device_next, opus_next;
    int hold, held;
} HleAudio;
/* audio's host side: locks and condition variables, which a save state never holds (hle.h's g_host) */
typedef struct {
    PMutex lk; /* the audio outs, and the device's volume */
    /* a game pause: hold asks the clock's thread to stop at the top of its turn, held says it has, both under lk */
    PCond hold_cv;
} HostAudio;

/* ---- audren.c */
/* an open renderer, its object's u[1] its slot plus 1 */
typedef struct {
    SvcObj* o; /* set while the slot is taken */
    ARState* s;
    PAudio* out;    /* opened on the first frame with a device sink; NULL before, or without a host device */
    int out_failed; /* the host had no output for it: not asked again */
    int out_started;
} ARRen;
typedef struct {
    /* the renderers, and every renderer's state: an update and a frame never run at once; and the output device's
       volume (audio.c) on each renderer's host output */
    ARRen rens[SNX_AUDREN_MAX];
    u32 ren_next;
    float gain;
    i16 stereo[2 * 1024]; /* a frame is at most 1024 samples (audren_state_new); only the clock's thread */
} HleAudren;
/* audren's host side: locks and condition variables, which a save state never holds (hle.h's g_host) */
typedef struct {
    PMutex lk;
} HostAudren;

/* ---- ldr.c */
#define RO_LIMIT    64
#define RO_SESSIONS 8 /* ldr:ro and ro:1 sessions: a process has one of each, which nn::ro opens once */
typedef struct {
    MemCodeMap map;
    u8 id[32];
} RoNro;
typedef struct {
    u64 address;
    u32 count;
    u8* hashes; /* count hashes, the NRR's own allocation */
} RoNrr;
typedef struct {
    u32 obj;
    int initialized, kind;
    RoNro nro[RO_LIMIT];
    RoNrr nrr[RO_LIMIT];
} RoState;
typedef struct {
    u32 owner;   /* the obj of the session that initialized, 0 for none */
    RoState states[RO_SESSIONS];
    u32 state_next;
} HleLdr;
/* ldr's host side: locks and condition variables, which a save state never holds (hle.h's g_host) */
typedef struct {
    PMutex lock; /* also serializes cloned/domain sessions and initialization across ldr:ro and ro:1 */
} HostLdr;

/* ---- misc.c */
#define APM_CONFIGURATION 0x00020003u /* Config7, the default of both modes */
#define APM_MODES         2           /* nn::apm::PerformanceMode: Normal, Boost */
#define MM_REQUESTS       32
#define MM_SESSIONS       64 /* mm:u sessions: one per GetServiceHandle, which a game makes a few of */
#define NFC_USERS         16 /* nfp's and nfc's IUsers: one each for a game's NFC library */
#define AOC_MAX           2047 /* add-on content indexes, 1 to 2047 */
typedef struct {
    u32 used, id, module, current;
} MmRequest;
/* an mm:u session's clock requests */
typedef struct {
    u32 obj;
    u32 next_id;
    MmRequest req[MM_REQUESTS];
} Mm;
/* an IUser: its nn::nfc::State (0 not initialized, 1 initialized), and the one availability-change event it hands
   out every time, never signaled since no device comes or goes */
typedef struct {
    u32 obj;
    u32 state;
    u32 availability;
} NfcUser;
typedef struct {
    u32 apm_config[APM_MODES]; /* each mode's configuration, as a session or SetCpuBoostMode last set it */
    Mm mms[MM_SESSIONS];
    u32 mm_next;
    NfcUser nfc_users[NFC_USERS];
    u32 nfc_user_next;
    int aoc_scanned;
    i32 aoc_count, aoc_index[AOC_MAX];
    u32 irs_shm;
} HleMisc;
/* misc's host side: locks and condition variables, which a save state never holds (hle.h's g_host) */
typedef struct {
    PMutex mm_lock;
    /* the add-on contents installed, scanned once */
    PMutex aoc_lock;
    /* irs's shared memory, made at the first ask */
    PMutex irs_lock;
} HostMisc;

/* ---- pl.c */
#define PL_FONTS 6
typedef struct {
    u32 shm;
    u32 font_off[PL_FONTS], font_size[PL_FONTS]; /* where each font starts (past its header), and its size */
} HlePl;

/* ---- set.c */
/* the system settings set:sys keeps for the run, each at first as on a console fresh from the factory (Eden's
   DefaultSystemSettings), the keyboard layout the language's */
typedef struct {
    i32 color_set; /* nn::settings::system::ColorSet: 0 BasicWhite, 1 BasicBlack */
    float vibration;
    u32 keyboard_layout;
    u8 lock_screen, quest, battery_percentage, speaker_auto_mute, layout_set;
} HleSet;
/* sm.c's: the names of the stand-ins for services the runtime doesn't have, kept for the process (IPC's crash ring
   keeps the names of objects already gone) */
#define STAND_IN_NAMES 256 /* more names than Horizon has services */
typedef struct {
    char name[STAND_IN_NAMES][9];
    int n;
} HleSm;
/* sm's host side: locks and condition variables, which a save state never holds (hle.h's g_host) */
typedef struct {
    PMutex lk;
} HostSm;

/* ---- time.c */
#define TIME_OBJS 1024 /* time's objects: a service per session, and the clocks and time zone services it hands out */
/* a time zone rule as time.c writes it into the game's nn::time::TimeZoneRule */
typedef struct {
    char magic[4];  /* "SNXZ" */
    i32 utc_offset; /* seconds east of UTC */
    char abbreviation[8];
} TimeRule;
/* each time object's: 1 for time:u's and what it hands out, which may write nothing */
typedef struct {
    u32 obj;
    int read_only;
} TimeObj;
typedef struct {
    u32 shm;
    u8 source_id[16]; /* the steady clock's: the console makes a new one when its RTC resets */
    i64 base_ns;      /* the steady clock at the guest clock's start */
    /* every system clock's context: its offset, and the steady clock's time (s) it was taken at */
    i64 context_offset, context_start;
    TimeRule rule; /* the device's */
    char location[0x24];
    TimeObj objs[TIME_OBJS];
    u32 obj_next;
} HleTime;
#endif
