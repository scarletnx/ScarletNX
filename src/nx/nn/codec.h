/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_NX_NN_CODEC_H
#define SNX_NX_NN_CODEC_H
#include "common/cpu/aarch64/cpu.h"

/* The native nn::codec hardware Opus decoder (codec.c, codec.hook). FinalizeHardwareOpusDecoder and
   ResetHardwareOpusDecoder carry the same symbols in the Ounce's SDK, whose hooks (hle/hook/codec.c) hand over here
   the decoders that are not the Ounce's */
void nncodec_hwopus_finalize(CPU* c, u64 decoder);
void nncodec_hwopus_reset(CPU* c, u64 decoder);
#endif
