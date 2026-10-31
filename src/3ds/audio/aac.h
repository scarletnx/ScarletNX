/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_CTR_AAC_H
#define SNX_CTR_AAC_H
#include "3ds/memory/ctr_memory.h"
#include <stdint.h>

/* The DSP's AAC decoder, which an application drives through the binary pipe (aac.c): a message of this many bytes in,
   the same size back */
#define SNX_CTR_AAC_MESSAGE 32u
typedef struct SnxCtrAac SnxCtrAac;

SnxCtrAac* snx_ctr_aac_new(SnxCtrMemory* memory);
void snx_ctr_aac_free(SnxCtrAac* aac);
/* One decoder message (3dbrew's DSP binary pipe): the request answered in response. Decode reads ADTS frames from
   FCRAM and writes the two channels' PCM16 to FCRAM, the response saying the stream's rate and channels and the
   samples a channel got. A decode that fails answers with the DSP's fallback (48 kHz stereo, 1024 samples) and
   writes nothing, as Azahar's does */
void snx_ctr_aac_request(SnxCtrAac* aac, const uint8_t request[SNX_CTR_AAC_MESSAGE],
                         uint8_t response[SNX_CTR_AAC_MESSAGE]);
#endif
