/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* OpenSL ES (libOpenSLES) as HLE. Its interface IDs are objects: each SL_IID_* is a pointer to the interface's ID,
   which an app passes back to name the interface. The engine is not there yet (audio onto SDL is to come), so
   slCreateEngine reports SL_RESULT_FEATURE_UNSUPPORTED, which an app handles as a device without OpenSL ES */
#include "android/bridge/hle.h"
#include "android/bridge/hle_impl.h"

#define IDS(F)                                                                                                         \
    F(NULL) F(OBJECT) F(AUDIOIODEVICECAPABILITIES) F(LED) F(VIBRA) F(METADATAEXTRACTION) F(METADATATRAVERSAL)          \
    F(DYNAMICSOURCE) F(OUTPUTMIX) F(PLAY) F(PREFETCHSTATUS) F(PLAYBACKRATE) F(SEEK) F(RECORD) F(EQUALIZER) F(VOLUME)   \
    F(DEVICEVOLUME) F(BUFFERQUEUE) F(PRESETREVERB) F(ENVIRONMENTALREVERB) F(EFFECTSEND) F(3DGROUPING) F(3DCOMMIT)      \
    F(3DLOCATION) F(3DDOPPLER) F(3DSOURCE) F(3DMACROSCOPIC) F(MUTESOLO) F(DYNAMICINTERFACEMANAGEMENT) F(MIDIMESSAGE)   \
    F(MIDIMUTESOLO) F(MIDITEMPO) F(MIDITIME) F(AUDIODECODERCAPABILITIES) F(AUDIOENCODERCAPABILITIES) F(AUDIOENCODER)  \
    F(BASSBOOST) F(PITCH) F(RATEPITCH) F(VIRTUALIZER) F(VISUALIZATION) F(ENGINE) F(ENGINECAPABILITIES) F(THREADSYNC)  \
    F(ANDROIDEFFECT) F(ANDROIDEFFECTSEND) F(ANDROIDEFFECTCAPABILITIES) F(ANDROIDCONFIGURATION)                         \
    F(ANDROIDSIMPLEBUFFERQUEUE) F(ANDROIDBUFFERQUEUESOURCE) F(ANDROIDACOUSTICECHOCANCELLATION)                         \
    F(ANDROIDAUTOMATICGAINCONTROL) F(ANDROIDNOISESUPPRESSION)

/* each ID's 16 bytes (their values are the interface's, and only their addresses matter here), and its pointer */
#define ID_OBJECT(name)                                                                                                \
    static const unsigned char id_##name[16] = {#name[0]};                                                             \
    static const void* const sl_iid_##name = id_##name;
IDS(ID_OBJECT)
#define ID_ENTRY(name) {"SL_IID_" #name, NULL, (void*)&sl_iid_##name},

static void create_engine(CPU* c, void* data) {
    (void)data;
    X(0) = 12; /* SL_RESULT_FEATURE_UNSUPPORTED */
}

const HleSymbol hle_sles[] = {
    IDS(ID_ENTRY)
    {"slCreateEngine", create_engine, NULL},
    {"", NULL, NULL},
};
