/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_CTR_APT_H
#define SNX_CTR_APT_H
#include "3ds/ipc/ctr_ipc.h"
void snx_ctr_apt_dispatch(SnxCtrIpc* c);
/* APT's signals (Azahar SignalType) */
enum { SNX_CTR_APT_REQUEST = 2, SNX_CTR_APT_RESPONSE = 3, SNX_CTR_APT_MESSAGE = 5, SNX_CTR_APT_WAKEUP_BY_EXIT = 0xa };
/* A parameter from sender for the application, over the one waiting (Azahar CancelAndSendParameter): its object
   retained for it (0 for none), its buffer of at most 0x1000 bytes; the application's parameter event is signaled */
void snx_ctr_apt_send(struct SnxCtrProcess* p, uint32_t sender, uint32_t signal, uint32_t object,
                      const unsigned char* buffer, uint32_t size);
/* the library applet APT registered has closed: the application may prepare another */
void snx_ctr_apt_library_closed(struct SnxCtrProcess* p);
#endif
