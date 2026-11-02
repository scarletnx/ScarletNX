/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_SCHEDULING_H
#define SNX_SCHEDULING_H

typedef enum { SNX_WORK_GUEST, SNX_WORK_GPU, SNX_WORK_RECORD, SNX_WORK_COUNT } SnxThreadWork;
/* Apply the startup policy to the calling host thread, once before it runs work. Guest priorities stay in thread.c. */
void snx_thread_performance(SnxThreadWork work);

#endif
